#include "fusion/fusionregistration.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include <QCoreApplication>
#include <QString>
#include <QVector>

#include "attributeregistry.h"
#include "calculations/anglehelper.h"
#include "calculations/attributecalculations.h"
#include "calculations/registration.h"
#include "calculations/timefithelper.h"
#include "calculations/trackhelper.h"
#include "engine/calculationprogress.h"
#include "fusion/fusion.h"
#include "fusion/orientation.h"
#include "fusion/solverthreads.h"
#include "sensorconfiguration.h"
#include "sessiondata.h"

// Sensor fusion as registered calculations: a thin adapter between the engine
// and the kernel (fusion.h). The adapter copies values and nothing else: every
// validation rule, unit conversion and message belongs to the kernel, where
// the golden tests hold it to the reference.

using namespace FlySight;

namespace {

constexpr char kSensor[] = "Fusion";

// The thirty-three measurement outputs of the fit and the array of the
// kernel's result behind each: the seventeen of the state, then the four
// accuracies, then the upper triangles of the position and velocity covariance
// blocks. One table serves the declaration and the publication, so the two
// cannot drift apart.
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
    { "qw",    &Fusion::Result::qw },
    { "headingAcc", &Fusion::Result::headingAcc },
    { "tiltAcc",    &Fusion::Result::tiltAcc },
    { "accHAcc",    &Fusion::Result::accHAcc },
    { "accDAcc",    &Fusion::Result::accDAcc },
    { "posCovNN",   &Fusion::Result::posCovNN },
    { "posCovNE",   &Fusion::Result::posCovNE },
    { "posCovND",   &Fusion::Result::posCovND },
    { "posCovEE",   &Fusion::Result::posCovEE },
    { "posCovED",   &Fusion::Result::posCovED },
    { "posCovDD",   &Fusion::Result::posCovDD },
    { "velCovNN",   &Fusion::Result::velCovNN },
    { "velCovNE",   &Fusion::Result::velCovNE },
    { "velCovND",   &Fusion::Result::velCovND },
    { "velCovEE",   &Fusion::Result::velCovEE },
    { "velCovED",   &Fusion::Result::velCovED },
    { "velCovDD",   &Fusion::Result::velCovDD }
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

// The four configuration attributes the fit reads and the member of the
// kernel's ImuConfiguration each one fills, in input order. One table serves
// the declaration, the hand-over and the constant defaults. The receiver's
// model and rate and the other sensors' rates are not inputs: nothing in the
// fit uses them, and declaring them would make a stored fit stale when one
// changed.
struct ConfigurationInput {
    const char *key;
    double Fusion::ImuConfiguration::*value;
};

constexpr ConfigurationInput kConfigurationInputs[] = {
    { SensorConfiguration::AccelFsG,   &Fusion::ImuConfiguration::accelFsG },
    { SensorConfiguration::GyroFsDegS, &Fusion::ImuConfiguration::gyroFsDegS },
    { SensorConfiguration::AccelOdrHz, &Fusion::ImuConfiguration::accelOdrHz },
    { SensorConfiguration::GyroOdrHz,  &Fusion::ImuConfiguration::gyroOdrHz }
};

} // namespace

// ---- the tables, as the public header exposes them --------------------------
// Three functions over kFitInputs, kConfigurationInputs and kFitOutputs for
// the registration below and for the tooling (fusion_runner), so that the
// tool feeds the kernel what computeFit() feeds it by construction: one
// table, one assembly.

// The eighteen measurements of kFitInputs, then the four origin attributes,
// then the four configuration attributes of kConfigurationInputs. Everything
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
    for (const ConfigurationInput &input : kConfigurationInputs)
        inputs.append(CalcInput::attribute(input.key));
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

    // The configuration as numbers. A stored, hand-edited value that is not a
    // number must not silently mean 0: it stays the kernel's NaN.
    for (const ConfigurationInput &input : kConfigurationInputs) {
        bool ok = false;
        const double value = attribute(QString::fromLatin1(input.key)).toDouble(&ok);
        if (ok)
            channels.imuConfiguration.*(input.value) = value;
    }
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
// and asking again with the same inputs runs nothing. A success whose
// covariance could not be computed has no accuracies: those sixteen (the four
// accuracies and the twelve covariance entries) stay unset too, since an
// available measurement has samples (the record refuses one without), and the
// diagnostics say why.
CalculationResult publish(const Fusion::Result &fit)
{
    CalculationResult result;
    result.setAttribute(SessionKeys::FusionDiagnostics, fit.diagnosticsJson);
    if (fit.outcome != Fusion::Outcome::Succeeded) {
        result.setReason(fit.reason);
        return result;
    }
    for (const FitOutput &output : kFitOutputs) {
        const QVector<double> &samples = fit.*(output.samples);
        if (!samples.isEmpty())
            result.setMeasurement(kSensor, output.name, samples);
    }
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
    // On the executor's below-normal worker the solver's helper threads must
    // run below normal too, or they starve it (solverthreads.h)
    Fusion::Result fit;
    Fusion::runWithSolverThreadsAtCallerPriority([&] {
        fit = Fusion::run(
            channels,
            [&facility](const QString &text) { facility.report(text); },
            [&facility] { return facility.isCancelled(); });
    });

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

// Fusion/velH: horizontal speed, the magnitude of velN and velE, as GNSS/velH
// is of the GNSS components. On demand, but its inputs exist only once the
// fit has published, so it appears with the fit through ordinary invalidation
// and never starts one.
void registerHorizontalSpeed(CalculationRegistry &registry)
{
    CalculationDescriptor d;
    d.id = QStringLiteral("builtin.fusion.velH");
    d.inputs = {
        CalcInput::measurement(kSensor, "velN"),
        CalcInput::measurement(kSensor, "velE")
    };
    d.outputs = { DependencyKey::measurement(kSensor, "velH") };
    d.compute = [](const EvaluationContext &ctx) -> CalculationResult {
        const QVector<double> velN = ctx.measurement(kSensor, "velN");
        const QVector<double> velE = ctx.measurement(kSensor, "velE");
        if (velN.size() != velE.size())
            return CalculationResult::unavailable();

        QVector<double> velH;
        velH.reserve(velN.size());
        for (int i = 0; i < velN.size(); ++i)
            velH.append(std::sqrt(velN[i]*velN[i] + velE[i]*velE[i]));
        return CalculationResult().setMeasurement(kSensor, "velH", velH);
    };
    Calculations::addCalculation(registry, d);
}

// Fusion/vel: total speed, the magnitude of velH and velD, as GNSS/vel is of
// GNSS/velH and GNSS/velD. It reads Fusion/velH, which waits on the fit, so
// it too appears with the fit and never starts one.
void registerTotalSpeed(CalculationRegistry &registry)
{
    CalculationDescriptor d;
    d.id = QStringLiteral("builtin.fusion.vel");
    d.inputs = {
        CalcInput::measurement(kSensor, "velH"),
        CalcInput::measurement(kSensor, "velD")
    };
    d.outputs = { DependencyKey::measurement(kSensor, "vel") };
    d.compute = [](const EvaluationContext &ctx) -> CalculationResult {
        const QVector<double> velH = ctx.measurement(kSensor, "velH");
        const QVector<double> velD = ctx.measurement(kSensor, "velD");
        if (velH.size() != velD.size())
            return CalculationResult::unavailable();

        QVector<double> vel;
        vel.reserve(velH.size());
        for (int i = 0; i < velH.size(); ++i)
            vel.append(std::sqrt(velH[i]*velH[i] + velD[i]*velD[i]));
        return CalculationResult().setMeasurement(kSensor, "vel", vel);
    };
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

// Fusion/z: elevation above the ground, the origin's height less the fused
// down position less the ground elevation, so that it reads as GNSS/z does
// (hMSL - _GROUND_ELEV). On demand and waiting on the fit like accH. Unlike
// GNSS/z it does not warn when an attribute is not a number: nothing in this
// library logs, and the engine reports the value as unavailable.
void registerElevation(CalculationRegistry &registry)
{
    CalculationDescriptor d;
    d.id = QStringLiteral("builtin.fusion.z");
    d.inputs = {
        CalcInput::measurement(kSensor, "down"),
        CalcInput::attribute(SessionKeys::LocalOriginHmsl),
        CalcInput::attribute(SessionKeys::GroundElev)
    };
    d.outputs = { DependencyKey::measurement(kSensor, "z") };
    d.compute = [](const EvaluationContext &ctx) -> CalculationResult {
        const QVector<double> down = ctx.measurement(kSensor, "down");
        bool originOk = false;
        bool groundOk = false;
        const double originHmsl = ctx.attribute(SessionKeys::LocalOriginHmsl).toDouble(&originOk);
        const double groundElev = ctx.attribute(SessionKeys::GroundElev).toDouble(&groundOk);
        if (!originOk || !groundOk || down.isEmpty())
            return CalculationResult::unavailable();

        QVector<double> z;
        z.reserve(down.size());
        for (const double sample : down)
            z.append((originHmsl - sample) - groundElev);
        return CalculationResult().setMeasurement(kSensor, "z", z);
    };
    Calculations::addCalculation(registry, d);
}

using TrackFunction = std::optional<QVector<double>> (*)(const Calculations::TrackSamples &);

// Fusion/<name>: a track-relative acceleration of the fused velocity and
// acceleration, by the GNSS definition, with the inputs of the GNSS ones in
// their order. On demand and waiting on the fit like accH.
void registerTrackAcceleration(CalculationRegistry &registry, const char *name, TrackFunction track)
{
    const QString measurement = QString::fromLatin1(name);

    CalculationDescriptor d;
    d.id = QStringLiteral("builtin.fusion.") + measurement;
    d.inputs = {
        CalcInput::measurement(kSensor, "accN"),
        CalcInput::measurement(kSensor, "accE"),
        CalcInput::measurement(kSensor, "accD"),
        CalcInput::measurement(kSensor, "velN"),
        CalcInput::measurement(kSensor, "velE"),
        CalcInput::measurement(kSensor, "velD"),
        CalcInput::attribute(SessionKeys::WindN),
        CalcInput::attribute(SessionKeys::WindE)
    };
    d.outputs = { DependencyKey::measurement(kSensor, measurement) };
    d.compute = [measurement, track](const EvaluationContext &ctx) -> CalculationResult {
        const std::optional<QVector<double>> values = track(Calculations::trackSamples(ctx, kSensor));
        if (!values)
            return CalculationResult::unavailable();
        return CalculationResult().setMeasurement(kSensor, measurement, *values);
    };
    Calculations::addCalculation(registry, d);
}

// The orientation attribute. Its definition goes into the attribute registry
// once per process: the tests call the entry point for more than one
// calculation registry, the attribute registry cannot remove a
// definition, and a second one would list the attribute twice in the Add
// Column dialog. Its constant default goes into every registry given. The
// choices and the default come from Fusion::Orientation, so nothing here
// spells a token.
void registerOrientation(CalculationRegistry &registry)
{
    const QString key = QString::fromLatin1(SessionKeys::Orientation);
    AttributeRegistry &attributes = AttributeRegistry::instance();
    if (!attributes.findByKey(key)) {
        QVector<AttributeChoice> choices;
        for (const Fusion::Orientation &orientation : Fusion::Orientation::all())
            choices.append({ orientation.token(), orientation.label() });
        attributes.registerAttribute({ QStringLiteral("Session"), QStringLiteral("Orientation"), key,
                                       AttributeFormatType::Choice, true, QString(), choices });
    }
    Calculations::addConstantDefault(registry, key, Fusion::Orientation::defaultOrientation().token());
}

// The constant defaults of the four configuration attributes the fit reads,
// in input order: the text of the vocabulary, a QString like the value the
// importer stores, so that a stored value and a default of the same text read
// the same. A
// recording that states a key reads its own value; the defaults are what
// every recording on disk reads, since the firmware that wrote them states
// none. Nothing here spells a key or a value.
void registerConfigurationDefaults(CalculationRegistry &registry)
{
    for (const ConfigurationInput &input : kConfigurationInputs) {
        const QString key = QString::fromLatin1(input.key);
        Calculations::addConstantDefault(registry, key, SensorConfiguration::defaultValue(key).value());
    }
}

constexpr double kDegreesPerRadian = 57.295779513082320876798;

// Heading, pitch and roll of the body frame, in degrees, for one sample.
struct BodyAngles {
    double heading, pitch, roll;
};

// The angles of one sample of the fit's quaternion (x, y, z, w; Hamilton,
// device to north-east-down, as fillOutputChannels() publishes it: R v_device
// = v_NED) on the body frame whose body-to-device rotation is `c`. The body's
// rotation to north-east-down is M = R C, and its aircraft Euler angles are
// heading = atan2(M10, M00), pitch = asin(-M20), roll = atan2(M21, M22): the
// convention of the fit's own roll, pitch and yaw, which these are when the
// body frame is the device frame. Nothing for a quaternion that is not finite
// or has no length: one NaN would reach every later heading through the
// unwrap.
std::optional<BodyAngles> bodyAngles(double qx, double qy, double qz, double qw,
                                     const Fusion::Orientation::Matrix &c)
{
    const double norm = std::sqrt(qx*qx + qy*qy + qz*qz + qw*qw);
    if (!std::isfinite(norm) || norm == 0.0)
        return std::nullopt;
    const double x = qx / norm, y = qy / norm, z = qz / norm, w = qw / norm;

    const double r[3][3] = {
        { 1 - 2*(y*y + z*z), 2*(x*y - z*w),     2*(x*z + y*w) },
        { 2*(x*y + z*w),     1 - 2*(x*x + z*z), 2*(y*z - x*w) },
        { 2*(x*z - y*w),     2*(y*z + x*w),     1 - 2*(x*x + y*y) }
    };
    const auto m = [&r, &c](int row, int column) {
        return r[row][0]*c[0][column] + r[row][1]*c[1][column] + r[row][2]*c[2][column];
    };

    BodyAngles angles;
    angles.heading = std::atan2(m(1, 0), m(0, 0)) * kDegreesPerRadian;
    // Rounding may take |M20| a hair past 1 near the vertical
    angles.pitch = std::asin(std::clamp(-m(2, 0), -1.0, 1.0)) * kDegreesPerRadian;
    angles.roll = std::atan2(m(2, 1), m(2, 2)) * kDegreesPerRadian;
    // atan2 gives -180 for a negative zero; roll's range is (-180, 180]
    if (angles.roll <= -180.0)
        angles.roll += 360.0;
    return angles;
}

// Fusion/bodyHeading, bodyPitch and bodyRoll from the fit's quaternion and the
// orientation attribute, published together. It may run on any thread while
// another session evaluates the same descriptor, so it keeps no state, and
// like everything in this library it logs nothing. Unavailable for a token
// that is not an orientation, for empty or unequal quaternion arrays, and for
// a quaternion sample that is not finite or has no length. Heading is a
// compass heading: measured from north and not referenced to the course
// reference (GNSS/course is), so the attitude needs nothing of GNSS.
CalculationResult computeAttitude(const EvaluationContext &ctx)
{
    const std::optional<Fusion::Orientation> orientation =
        Fusion::Orientation::fromToken(ctx.attribute(SessionKeys::Orientation).toString());
    if (!orientation)
        return CalculationResult::unavailable();

    const QVector<double> qx = ctx.measurement(kSensor, "qx");
    const QVector<double> qy = ctx.measurement(kSensor, "qy");
    const QVector<double> qz = ctx.measurement(kSensor, "qz");
    const QVector<double> qw = ctx.measurement(kSensor, "qw");
    const qsizetype n = qx.size();
    if (n == 0 || qy.size() != n || qz.size() != n || qw.size() != n)
        return CalculationResult::unavailable();

    const Fusion::Orientation::Matrix c = orientation->bodyToDevice();
    QVector<double> heading, pitch, roll;
    heading.reserve(n);
    pitch.reserve(n);
    roll.reserve(n);
    for (qsizetype i = 0; i < n; ++i) {
        const std::optional<BodyAngles> angles = bodyAngles(qx[i], qy[i], qz[i], qw[i], c);
        if (!angles)
            return CalculationResult::unavailable();
        heading.append(angles->heading);
        pitch.append(angles->pitch);
        roll.append(angles->roll);
    }

    heading = Calculations::unwrapDegrees(heading);

    return CalculationResult()
        .setMeasurement(kSensor, "bodyHeading", heading)
        .setMeasurement(kSensor, "bodyPitch", pitch)
        .setMeasurement(kSensor, "bodyRoll", roll);
}

// Fusion/bodyHeading, bodyPitch, bodyRoll: the attitude of the body frame
// that the orientation attribute defines. On demand and waiting on the fit
// like accH. The fit does not read the attribute, so an orientation edit
// recomputes the angles and never refits. The quaternion and the orientation
// are its only inputs: every fitted recording has an attitude.
void registerAttitude(CalculationRegistry &registry)
{
    CalculationDescriptor d;
    d.id = QStringLiteral("builtin.fusion.attitude");
    d.inputs = {
        CalcInput::measurement(kSensor, "qx"),
        CalcInput::measurement(kSensor, "qy"),
        CalcInput::measurement(kSensor, "qz"),
        CalcInput::measurement(kSensor, "qw"),
        CalcInput::attribute(SessionKeys::Orientation)
    };
    d.outputs = {
        DependencyKey::measurement(kSensor, "bodyHeading"),
        DependencyKey::measurement(kSensor, "bodyPitch"),
        DependencyKey::measurement(kSensor, "bodyRoll")
    };
    d.compute = computeAttitude;
    Calculations::addCalculation(registry, d);
}

} // namespace

void Fusion::registerFusionCalculations(CalculationRegistry &registry)
{
    // The vocabulary before its reader
    registerConfigurationDefaults(registry);
    registerFit(registry);
    // The vocabulary before its reader: vel reads velH
    registerHorizontalSpeed(registry);
    registerTotalSpeed(registry);
    registerHorizontalAcceleration(registry);
    registerSystemTime(registry);
    registerElevation(registry);
    registerTrackAcceleration(registry, "accAlongTrack", Calculations::alongTrackAcceleration);
    registerTrackAcceleration(registry, "accCrossTrack", Calculations::crossTrackAcceleration);
    // The vocabulary before its reader
    registerOrientation(registry);
    registerAttitude(registry);
}
