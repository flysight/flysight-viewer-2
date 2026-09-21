#include "fakesessionstate.h"

#include <optional>

using namespace FlySight;

namespace FlySightTest {

// ------------------------------------------------------------ FakeSessionState

void FakeSessionState::setAttribute(const QString &key, const QVariant &v)
{
    m_attributes.insert(key, v);
}

void FakeSessionState::removeAttribute(const QString &key)
{
    m_attributes.remove(key);
}

void FakeSessionState::setMeasurement(const QString &sensor, const QString &name,
                                      const QVector<double> &v, const QString &unit)
{
    m_measurements.insert(qMakePair(sensor, name), v);
    m_units.insert(qMakePair(sensor, name), unit);
}

void FakeSessionState::removeMeasurement(const QString &sensor, const QString &name)
{
    m_measurements.remove(qMakePair(sensor, name));
    m_units.remove(qMakePair(sensor, name));
}

void FakeSessionState::setUnit(const QString &sensor, const QString &name, const QString &unit)
{
    m_units.insert(qMakePair(sensor, name), unit);
}

QSet<DependencyKey> FakeSessionState::setAttribute(CalculationEngine &e, const QString &key,
                                                   const QVariant &v)
{
    setAttribute(key, v);
    return e.attributeChanged(key);
}

QSet<DependencyKey> FakeSessionState::removeAttribute(CalculationEngine &e, const QString &key)
{
    removeAttribute(key);
    return e.attributeChanged(key);
}

QSet<DependencyKey> FakeSessionState::setMeasurement(CalculationEngine &e, const QString &sensor,
                                                     const QString &name, const QVector<double> &v,
                                                     const QString &unit)
{
    setMeasurement(sensor, name, v, unit);
    QSet<DependencyKey> names = e.sourceMeasurementChanged(sensor, name);
    names.unite(e.sourceUnitChanged(sensor, name));
    return names;
}

QSet<DependencyKey> FakeSessionState::removeMeasurement(CalculationEngine &e, const QString &sensor,
                                                        const QString &name)
{
    removeMeasurement(sensor, name);
    QSet<DependencyKey> names = e.sourceMeasurementChanged(sensor, name);
    names.unite(e.sourceUnitChanged(sensor, name));
    return names;
}

QSet<DependencyKey> FakeSessionState::setUnit(CalculationEngine &e, const QString &sensor,
                                              const QString &name, const QString &unit)
{
    setUnit(sensor, name, unit);
    return e.sourceUnitChanged(sensor, name);
}

bool FakeSessionState::hasStoredAttribute(const QString &key) const
{
    ++m_reads;
    return m_attributes.contains(key);
}

QVariant FakeSessionState::storedAttribute(const QString &key) const
{
    ++m_reads;
    return m_attributes.value(key);
}

bool FakeSessionState::hasSourceMeasurement(const QString &sensor, const QString &name) const
{
    ++m_reads;
    return m_measurements.contains(qMakePair(sensor, name));
}

QVector<double> FakeSessionState::sourceMeasurement(const QString &sensor, const QString &name) const
{
    ++m_reads;
    return m_measurements.value(qMakePair(sensor, name));
}

QString FakeSessionState::sourceUnit(const QString &sensor, const QString &name) const
{
    ++m_reads;
    return m_units.value(qMakePair(sensor, name));
}

// ------------------------------------------------------ FakePreferenceProvider

void FakePreferenceProvider::set(const QString &key, const QVariant &v)
{
    m_values.insert(key, v);
}

void FakePreferenceProvider::set(CalculationRegistry &r, const QString &key, const QVariant &v)
{
    set(key, v);
    r.notifyPreferenceChanged(key);
}

QVariant FakePreferenceProvider::preferenceValue(const QString &key) const
{
    ++m_reads;
    return m_values.value(key);
}

// ------------------------------------------------------------------- Synthetic

namespace Synthetic {

namespace {

const QString kA = QStringLiteral("A");
const QString kB = QStringLiteral("B");
const QString kC = QStringLiteral("C");
const QString kX = QStringLiteral("X");
const QString kY = QStringLiteral("Y");
const QString kZ = QStringLiteral("Z");
const QString kW = QStringLiteral("W");
const QString kX2 = QStringLiteral("X2");
const QString kY2 = QStringLiteral("Y2");
const QString kP = QStringLiteral("p");
const QString kSensor = QStringLiteral("S");

} // namespace

CalculationDescriptor sum()
{
    CalculationDescriptor d;
    d.id = QStringLiteral("sum");
    d.inputs = {CalcInput::attribute(kA), CalcInput::attribute(kB)};
    d.outputs = {DependencyKey::attribute(kX)};
    d.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute(kX, ctx.attribute(kA).toInt() + ctx.attribute(kB).toInt());
    };
    return d;
}

CalculationDescriptor fallbackX()
{
    CalculationDescriptor d;
    d.id = QStringLiteral("fallbackX");
    d.inputs = {CalcInput::attribute(kC)};
    d.outputs = {DependencyKey::attribute(kX)};
    d.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute(kX, ctx.attribute(kC).toInt() * 10);
    };
    return d;
}

CalculationDescriptor constX()
{
    CalculationDescriptor d;
    d.id = QStringLiteral("constX");
    d.outputs = {DependencyKey::attribute(kX)};
    d.compute = [](const EvaluationContext &) { return CalculationResult().setAttribute(kX, -1); };
    return d;
}

CalculationDescriptor triple(EvaluationPolicy policy)
{
    CalculationDescriptor d;
    d.id = QStringLiteral("triple");
    d.inputs = {CalcInput::attribute(kX), CalcInput::preference(kP)};
    d.outputs = {DependencyKey::attribute(kY), DependencyKey::attribute(kZ), DependencyKey::attribute(kW)};
    d.policy = policy;
    d.compute = [](const EvaluationContext &ctx) {
        const int x = ctx.attribute(kX).toInt();
        const int p = ctx.preference(kP).toInt();
        CalculationResult r;
        r.setAttribute(kY, x + p);
        r.setAttribute(kZ, x * 2);
        if (x >= 0)
            r.setAttribute(kW, x * 3);      // otherwise W stays unavailable: a partial result
        return r;
    };
    return d;
}

CalculationDescriptor wAlt()
{
    CalculationDescriptor d;
    d.id = QStringLiteral("wAlt");
    d.inputs = {CalcInput::attribute(kZ)};
    d.outputs = {DependencyKey::attribute(kW)};
    d.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute(kW, ctx.attribute(kZ).toInt() + 1000);
    };
    return d;
}

CalculationDescriptor meas()
{
    CalculationDescriptor d;
    d.id = QStringLiteral("meas");
    d.inputs = {CalcInput::measurement(kSensor, QStringLiteral("m")), CalcInput::attribute(kY)};
    d.outputs = {DependencyKey::measurement(kSensor, QStringLiteral("d"))};
    d.compute = [](const EvaluationContext &ctx) {
        QVector<double> values = ctx.measurement(kSensor, QStringLiteral("m"));
        const int y = ctx.attribute(kY).toInt();
        for (double &v : values)
            v += y;
        return CalculationResult().setMeasurement(kSensor, QStringLiteral("d"), values, QStringLiteral("u"));
    };
    return d;
}

CalculationFamily neg()
{
    CalculationFamily f;
    f.id = QStringLiteral("neg");
    f.instantiate = [](const DependencyKey &name) -> std::optional<CalculationDescriptor> {
        const QString prefix = QStringLiteral("neg:");
        if (name.type != DependencyKey::Type::Attribute || !name.attributeKey.startsWith(prefix))
            return std::nullopt;
        const QString output = name.attributeKey;
        const QString source = output.mid(prefix.size());
        if (source.isEmpty())
            return std::nullopt;

        CalculationDescriptor d;
        d.id = output;      // instance key
        d.inputs = {CalcInput::attribute(source)};
        d.outputs = {DependencyKey::attribute(output)};
        d.compute = [output, source](const EvaluationContext &ctx) {
            return CalculationResult().setAttribute(output, -ctx.attribute(source).toInt());
        };
        return d;
    };
    return f;
}

CalculationDescriptor P()
{
    CalculationDescriptor d;
    d.id = QStringLiteral("P");
    d.inputs = {CalcInput::attribute(kY2)};
    d.outputs = {DependencyKey::attribute(kX2)};
    d.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute(kX2, ctx.attribute(kY2).toInt() + 1);
    };
    return d;
}

CalculationDescriptor Q()
{
    CalculationDescriptor d;
    d.id = QStringLiteral("Q");
    d.outputs = {DependencyKey::attribute(kX2)};
    d.compute = [](const EvaluationContext &) { return CalculationResult().setAttribute(kX2, 100); };
    return d;
}

CalculationDescriptor R()
{
    CalculationDescriptor d;
    d.id = QStringLiteral("R");
    d.inputs = {CalcInput::attribute(kX2)};
    d.outputs = {DependencyKey::attribute(kY2)};
    d.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute(kY2, ctx.attribute(kX2).toInt() + 1);
    };
    return d;
}

CalculationDescriptor S()
{
    CalculationDescriptor d;
    d.id = QStringLiteral("S");
    d.outputs = {DependencyKey::attribute(kY2)};
    d.compute = [](const EvaluationContext &) { return CalculationResult().setAttribute(kY2, 200); };
    return d;
}

void registerSharedWorld(CalculationRegistry &registry)
{
    registry.registerCalculation(sum());
    registry.registerCalculation(fallbackX());
    registry.registerCalculation(constX());
    registry.registerCalculation(triple());
    registry.registerCalculation(wAlt());
    registry.registerCalculation(meas());
    registry.registerFamily(neg());
    registry.registerCalculation(P());
    registry.registerCalculation(Q());
    registry.registerCalculation(R());
    registry.registerCalculation(S());
}

// ---- overlapping rings

namespace {

struct TangleSpec {
    const char *id;
    const char *output;
    QList<const char *> inputs;
    int constant;
};

const QList<TangleSpec> &tangleSpecs()
{
    static const QList<TangleSpec> specs = {
        {"oP", "OX", {"OY"}, 1},       {"oQ", "OX", {}, 100},
        {"oR", "OY", {"OX"}, 1},       {"oS", "OY", {"OX"}, 2},
        {"eA", "E1", {"OX"}, 1},
        {"eB", "E2", {"OY"}, 1},       {"eF", "E2", {}, 7},
        {"tT", "C1", {"A1", "B1"}, 0}, {"tC", "C1", {}, 50},
        {"tU", "A1", {"C1"}, 1},       {"tA", "A1", {}, 1},
        {"tV", "B1", {"C1"}, 1},       {"tB", "B1", {}, 2},
        {"k1", "K1", {"K2", "K3"}, 1}, {"c1", "K1", {}, 10},
        {"k2", "K2", {"K1", "K3"}, 1}, {"c2", "K2", {}, 20},
        {"k3", "K3", {"K1", "K2"}, 1}, {"c3", "K3", {}, 30},
    };
    return specs;
}

} // namespace

QStringList tangleIds()
{
    QStringList ids;
    for (const TangleSpec &spec : tangleSpecs())
        ids.append(QString::fromLatin1(spec.id));
    return ids;
}

CalculationDescriptor tangle(const QString &id)
{
    CalculationDescriptor d;
    for (const TangleSpec &spec : tangleSpecs()) {
        if (id != QLatin1String(spec.id))
            continue;
        const QString output = QString::fromLatin1(spec.output);
        QStringList inputs;
        for (const char *input : spec.inputs)
            inputs.append(QString::fromLatin1(input));
        const int constant = spec.constant;

        d.id = id;
        for (const QString &input : inputs)
            d.inputs.append(CalcInput::attribute(input));
        d.outputs = {DependencyKey::attribute(output)};
        d.compute = [output, inputs, constant](const EvaluationContext &ctx) {
            int value = constant;
            for (const QString &input : inputs)
                value += ctx.attribute(input).toInt();
            return CalculationResult().setAttribute(output, value);
        };
        break;
    }
    return d;
}

QList<DependencyKey> tangleNames()
{
    return {attr("OX"), attr("OY"), attr("E1"), attr("E2"), attr("C1"), attr("A1"), attr("B1"),
            attr("K1"), attr("K2"), attr("K3")};
}

void registerTangleWorld(CalculationRegistry &registry)
{
    for (const QString &id : tangleIds())
        registry.registerCalculation(tangle(id));
}

// ---- explicit calculations

namespace {

// output = input + add, on demand
CalculationDescriptor derived(const char *id, const char *input, const char *output, int add)
{
    const QString in = QString::fromLatin1(input);
    const QString out = QString::fromLatin1(output);
    CalculationDescriptor d;
    d.id = QString::fromLatin1(id);
    d.inputs = {CalcInput::attribute(in)};
    d.outputs = {DependencyKey::attribute(out)};
    d.compute = [in, out, add](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute(out, ctx.attribute(in).toInt() + add);
    };
    return d;
}

} // namespace

CalculationDescriptor expA()
{
    CalculationDescriptor d;
    d.id = QStringLiteral("expA");
    d.title = QStringLiteral("Explicit A");
    d.policy = EvaluationPolicy::Explicit;
    d.inputs = {CalcInput::attribute(QStringLiteral("EA_IN"))};
    d.outputs = {attr("EA1"), attr("EA2"), attr("EA_DIAG")};
    d.compute = [](const EvaluationContext &ctx) {
        const int in = ctx.attribute(QStringLiteral("EA_IN")).toInt();
        CalculationResult r;
        if (in < 0) {
            // A rejection is a result: the measurements-to-be are unavailable,
            // the diagnostics output and the reason say why.
            r.setAttribute(QStringLiteral("EA_DIAG"), QStringLiteral("rejected"));
            r.setReason(QStringLiteral("negative input"));
            return r;
        }
        r.setAttribute(QStringLiteral("EA1"), in + 1);
        r.setAttribute(QStringLiteral("EA2"), in * 2);
        r.setAttribute(QStringLiteral("EA_DIAG"), QStringLiteral("ok"));
        return r;
    };
    return d;
}

CalculationDescriptor derivA() { return derived("derivA", "EA1", "DA", 100); }
CalculationDescriptor derivA2() { return derived("derivA2", "DA", "DDA", 1000); }

CalculationDescriptor expB()
{
    CalculationDescriptor d;
    d.id = QStringLiteral("expB");
    d.title = QStringLiteral("Explicit B");
    d.policy = EvaluationPolicy::Explicit;
    d.inputs = {CalcInput::attribute(QStringLiteral("EA2")), CalcInput::attribute(QStringLiteral("EB_IN"))};
    d.outputs = {attr("EB1")};
    d.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute(QStringLiteral("EB1"),
                                                ctx.attribute(QStringLiteral("EA2")).toInt()
                                                    + ctx.attribute(QStringLiteral("EB_IN")).toInt());
    };
    return d;
}

CalculationDescriptor derivB() { return derived("derivB", "EB1", "DB", 1); }

QList<DependencyKey> explicitNames()
{
    return {attr("EA1"), attr("EA2"), attr("EA_DIAG"), attr("DA"), attr("DDA"), attr("EB1"), attr("DB")};
}

void registerExplicitWorld(CalculationRegistry &registry)
{
    registry.registerCalculation(expA());
    registry.registerCalculation(derivA());
    registry.registerCalculation(derivA2());
    registry.registerCalculation(expB());
    registry.registerCalculation(derivB());
}

} // namespace Synthetic

} // namespace FlySightTest
