// CalculationRegistry and the engine's value types: registration order,
// validation, family instances, isolation of private registries.

#include <optional>
#include <stdexcept>

#include <QSet>
#include <QtTest>

#include "engine/calctypes.h"
#include "engine/calculationdescriptor.h"
#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "engine/calculationresult.h"
#include "fakesessionstate.h"
#include "testmain.h"

using namespace FlySight;
using namespace FlySightTest;
using Synthetic::attr;
using Synthetic::measKey;

namespace {

CalculationDescriptor simple(const QString &id, const QString &output)
{
    CalculationDescriptor d;
    d.id = id;
    d.outputs = {DependencyKey::attribute(output)};
    d.compute = [output](const EvaluationContext &) { return CalculationResult().setAttribute(output, 1); };
    return d;
}

// Family accepting attribute names "<prefix><anything>".
CalculationFamily prefixFamily(const QString &id, const QString &prefix)
{
    CalculationFamily f;
    f.id = id;
    f.instantiate = [prefix](const DependencyKey &name) -> std::optional<CalculationDescriptor> {
        if (name.type != DependencyKey::Type::Attribute || !name.attributeKey.startsWith(prefix))
            return std::nullopt;
        const QString output = name.attributeKey;
        CalculationDescriptor d;
        d.id = output;
        d.outputs = {DependencyKey::attribute(output)};
        d.compute = [output](const EvaluationContext &) { return CalculationResult().setAttribute(output, 2); };
        return d;
    };
    return f;
}

// Family whose single instance "pair" has the two outputs "pair:lo" and "pair:hi".
CalculationFamily pairFamily()
{
    CalculationFamily f;
    f.id = QStringLiteral("pairs");
    f.instantiate = [](const DependencyKey &name) -> std::optional<CalculationDescriptor> {
        if (name.type != DependencyKey::Type::Attribute)
            return std::nullopt;
        if (name.attributeKey != QLatin1String("pair:lo") && name.attributeKey != QLatin1String("pair:hi"))
            return std::nullopt;
        CalculationDescriptor d;
        d.id = QStringLiteral("pair");
        d.outputs = {DependencyKey::attribute(QStringLiteral("pair:lo")),
                     DependencyKey::attribute(QStringLiteral("pair:hi"))};
        d.compute = [](const EvaluationContext &) {
            return CalculationResult().setAttribute(QStringLiteral("pair:lo"), 1)
                                      .setAttribute(QStringLiteral("pair:hi"), 2);
        };
        return d;
    };
    return f;
}

QStringList ids(const QList<CalculationInstance> &instances)
{
    QStringList result;
    for (const CalculationInstance &i : instances)
        result.append(i.instanceId);
    return result;
}

} // namespace

class CalcRegistryTest : public QObject {
    Q_OBJECT

private slots:
    void graphTypesAreHashKeys();
    void resultBundleAvailability();
    void candidatesInRegistrationOrder();
    void reRegisteringGoesToTheEnd();
    void registeredIdsInSequenceOrder();
    void validationRejections_data();
    void validationRejections();
    void duplicateIdRejected();
    void familyValidation();
    void invalidFamilyInstanceDoesNotMatch();
    void familyInstanceIsMemoized();
    void unregisterFamilyDropsInstances();
    void hasCandidateFor();
    void instanceLookup();
    void sourceConversionsAreSeparate();
    void localRegistryIsIsolated();
    void enrolment();
    void registrationDuringEvaluationRejected();

    // Registration-derived queries for the logbook column cache
    void staticDependenciesClosure();
    void declaredPreferenceKeys();
    void observersFire();

    // Opt-in source inputs for the Python plugin host
    void sourceInputsOptIn();

    // staticDependencies() and the source-input opt-in compose: an opted-in
    // source input is part of the static closure
    void staticDependenciesCoverOptInSourceInputs();
};

void CalcRegistryTest::graphTypesAreHashKeys()
{
    // Same strings, different kinds: all distinct.
    QVERIFY(GraphNode::storedAttribute("x") != GraphNode::preference("x"));
    QVERIFY(GraphNode::storedAttribute("x") != GraphNode::resolution(attr("x")));
    QVERIFY(GraphNode::storedAttribute("x") != GraphNode::result("x"));
    QVERIFY(GraphNode::sourceMeasurement("IMU", "wx") != GraphNode::resolution(measKey("IMU", "wx")));
    QVERIFY(GraphNode::sourceMeasurement("IMU", "wx") != GraphNode::sourceUnit("IMU", "wx"));
    QVERIFY(GraphNode::resolution(attr("x")) == GraphNode::resolution(attr("x")));
    QCOMPARE(GraphNode::resolution(measKey("IMU", "wx")).publicName(), measKey("IMU", "wx"));
    QCOMPARE(GraphNode::resolution(attr("x")).publicName(), attr("x"));

    QSet<GraphNode> nodes;
    nodes << GraphNode::storedAttribute("x") << GraphNode::preference("x")
          << GraphNode::resolution(attr("x")) << GraphNode::result("x")
          << GraphNode::sourceMeasurement("IMU", "wx") << GraphNode::sourceUnit("IMU", "wx")
          << GraphNode::resolution(measKey("IMU", "wx"))
          << GraphNode::storedAttribute("x");   // duplicate
    QCOMPARE(nodes.size(), 7);

    QVERIFY(CalcInput::attribute("x") != CalcInput::preference("x"));
    QVERIFY(CalcInput::measurement("S", "m") != CalcInput::sourceMeasurement("S", "m"));
    QVERIFY(CalcInput::sourceMeasurement("S", "m") != CalcInput::sourceUnit("S", "m"));
    QHash<CalcInput, int> inputs;
    inputs.insert(CalcInput::attribute("x"), 1);
    inputs.insert(CalcInput::preference("x"), 2);
    inputs.insert(CalcInput::measurement("S", "m"), 3);
    inputs.insert(CalcInput::sourceMeasurement("S", "m"), 4);
    inputs.insert(CalcInput::sourceUnit("S", "m"), 5);
    QCOMPARE(inputs.size(), 5);
    QCOMPARE(inputs.value(CalcInput::preference("x")), 2);
    QCOMPARE(inputs.value(CalcInput::sourceMeasurement("S", "m")), 4);
}

void CalcRegistryTest::resultBundleAvailability()
{
    const CalculationResult empty;
    QVERIFY(!empty.contains(attr("a")));
    QVERIFY(!empty.isAvailable(attr("a")));
    QVERIFY(!empty.isAvailable(measKey("S", "m")));
    QVERIFY(!CalculationResult::unavailable().isAvailable(attr("a")));
    QVERIFY(empty.setOutputs().isEmpty());

    CalculationResult r;
    r.setAttribute("a", QVariant());                // normalized to unavailable
    r.setMeasurement("S", "m", {});
    r.setAttribute("b", 5);
    r.setMeasurement("S", "n", {1.0, 2.0}, "u");
    r.setUnavailable(attr("c"));

    QVERIFY(r.contains(attr("a")));
    QVERIFY(!r.isAvailable(attr("a")));
    QVERIFY(r.contains(measKey("S", "m")));
    QVERIFY(!r.isAvailable(measKey("S", "m")));
    QVERIFY(r.isAvailable(attr("b")));
    QCOMPARE(r.attributeValue("b"), QVariant(5));
    QVERIFY(r.isAvailable(measKey("S", "n")));
    QCOMPARE(r.measurementValues("S", "n"), (QVector<double>{1.0, 2.0}));
    QCOMPARE(r.measurementUnit("S", "n"), QStringLiteral("u"));
    QVERIFY(r.contains(attr("c")));
    QVERIFY(!r.isAvailable(attr("c")));
    QCOMPARE(r.setOutputs().size(), 5);

    // An attribute and a measurement never alias.
    QVERIFY(!r.contains(measKey("b", "")));
}

void CalcRegistryTest::candidatesInRegistrationOrder()
{
    CalculationRegistry registry;
    QVERIFY(registry.registerCalculation(simple("c1", "k:x")));
    QVERIFY(registry.registerFamily(prefixFamily("fam", "k:")));
    QVERIFY(registry.registerCalculation(simple("c2", "k:x")));
    QVERIFY(registry.registerCalculation(simple("other", "unrelated")));

    // Plain calculations and the family instance interleave by sequence number.
    QCOMPARE(ids(registry.candidatesFor(attr("k:x"))), QStringList({"c1", "fam#k:x", "c2"}));
    QCOMPARE(ids(registry.candidatesFor(attr("k:y"))), QStringList({"fam#k:y"}));
    QCOMPARE(ids(registry.candidatesFor(attr("unrelated"))), QStringList({"other"}));
    QVERIFY(registry.candidatesFor(attr("nothing")).isEmpty());
    QVERIFY(registry.candidatesFor(measKey("k:x", "")).isEmpty());

    const QList<CalculationInstance> candidates = registry.candidatesFor(attr("k:x"));
    QCOMPARE(candidates.at(0).registrationId, QStringLiteral("c1"));
    QCOMPARE(candidates.at(0).descriptor->id, QStringLiteral("c1"));
    QCOMPARE(candidates.at(1).registrationId, QStringLiteral("fam"));
    QCOMPARE(candidates.at(1).descriptor->id, QStringLiteral("fam#k:x"));
    QVERIFY(!candidates.at(1).sourceConversion);
}

void CalcRegistryTest::reRegisteringGoesToTheEnd()
{
    CalculationRegistry registry;
    QVERIFY(registry.registerCalculation(simple("c1", "x")));
    QVERIFY(registry.registerCalculation(simple("c2", "x")));
    QVERIFY(registry.registerCalculation(simple("c3", "x")));
    QCOMPARE(ids(registry.candidatesFor(attr("x"))), QStringList({"c1", "c2", "c3"}));

    QVERIFY(registry.unregister("c1"));
    QVERIFY(!registry.contains("c1"));
    QVERIFY(!registry.unregister("c1"));    // already gone
    QVERIFY(registry.registerCalculation(simple("c1", "x")));
    QCOMPARE(ids(registry.candidatesFor(attr("x"))), QStringList({"c2", "c3", "c1"}));
}

void CalcRegistryTest::registeredIdsInSequenceOrder()
{
    CalculationRegistry registry;
    QVERIFY(registry.registeredIds().isEmpty());

    CalculationFamily conv;
    conv.id = QStringLiteral("conv");
    conv.instantiate = [](const DependencyKey &) -> std::optional<CalculationDescriptor> {
        return std::nullopt;
    };

    QVERIFY(registry.registerCalculation(simple("c1", "x")));
    QVERIFY(registry.registerFamily(prefixFamily("fam", "k:")));
    QVERIFY(registry.registerSourceConversion(conv));
    QVERIFY(registry.registerCalculation(simple("c2", "x")));
    QCOMPARE(registry.registeredIds(), QStringList({"c1", "fam", "conv", "c2"}));

    QVERIFY(!registry.isFamily("c1"));
    QVERIFY(registry.isFamily("fam"));
    QVERIFY(registry.isFamily("conv"));
    QVERIFY(!registry.isFamily("missing"));

    // Unregistering removes the id; re-registering moves it to the end.
    QVERIFY(registry.unregister("c1"));
    QCOMPARE(registry.registeredIds(), QStringList({"fam", "conv", "c2"}));
    QVERIFY(registry.registerCalculation(simple("c1", "x")));
    QCOMPARE(registry.registeredIds(), QStringList({"fam", "conv", "c2", "c1"}));

    QVERIFY(registry.unregister("fam"));
    QVERIFY(!registry.isFamily("fam"));
    QVERIFY(registry.registerFamily(prefixFamily("fam", "k:")));
    QCOMPARE(registry.registeredIds(), QStringList({"conv", "c2", "c1", "fam"}));

    // A rejected registration takes no place in the sequence.
    QVERIFY(!registry.registerCalculation(simple("c2", "y")));
    QCOMPARE(registry.registeredIds(), QStringList({"conv", "c2", "c1", "fam"}));
}

void CalcRegistryTest::validationRejections_data()
{
    QTest::addColumn<int>("rule");
    QTest::newRow("empty id") << 0;
    QTest::newRow("hash in id") << 1;
    QTest::newRow("no outputs") << 2;
    QTest::newRow("duplicate outputs") << 3;
    QTest::newRow("null compute") << 4;
    QTest::newRow("attribute output is own input") << 5;
    QTest::newRow("measurement output is own input") << 6;
    QTest::newRow("source measurement input") << 7;
    QTest::newRow("source unit input") << 8;
}

void CalcRegistryTest::validationRejections()
{
    QFETCH(int, rule);

    CalculationDescriptor d = simple("calc", "out");
    switch (rule) {
    case 0: d.id = QString(); break;
    case 1: d.id = QStringLiteral("ca#lc"); break;
    case 2: d.outputs.clear(); break;
    case 3: d.outputs.append(attr("out")); break;
    case 4: d.compute = nullptr; break;
    case 5: d.inputs = {CalcInput::attribute("out")}; break;
    case 6:
        d.outputs = {measKey("S", "m")};
        d.inputs = {CalcInput::measurement("S", "m")};
        break;
    case 7: d.inputs = {CalcInput::sourceMeasurement("S", "m")}; break;
    case 8: d.inputs = {CalcInput::sourceUnit("S", "m")}; break;
    }

    CalculationRegistry registry;
    QVERIFY(!registry.registerCalculation(d));
    QVERIFY(!registry.contains(d.id));
    QVERIFY(!registry.contains("calc"));
    QVERIFY(!registry.hasCandidateFor(attr("out")));
    QVERIFY(!registry.hasCandidateFor(measKey("S", "m")));

    // A preference input with the same key as an output is not a conflict.
    CalculationDescriptor ok = simple("calc", "out");
    ok.inputs = {CalcInput::preference("out")};
    QVERIFY(registry.registerCalculation(ok));
}

void CalcRegistryTest::duplicateIdRejected()
{
    CalculationRegistry registry;
    QVERIFY(registry.registerCalculation(simple("id", "x")));
    QVERIFY(!registry.registerCalculation(simple("id", "y")));          // as a calculation
    QVERIFY(!registry.registerFamily(prefixFamily("id", "k:")));        // as a family
    QVERIFY(!registry.registerSourceConversion(prefixFamily("id", "k:")));
    QVERIFY(registry.contains("id"));
    QVERIFY(!registry.hasCandidateFor(attr("y")));
    QVERIFY(!registry.hasCandidateFor(attr("k:1")));

    QVERIFY(registry.registerFamily(prefixFamily("fam", "k:")));
    QVERIFY(!registry.registerCalculation(simple("fam", "z")));
    QVERIFY(!registry.hasCandidateFor(attr("z")));
}

void CalcRegistryTest::familyValidation()
{
    CalculationRegistry registry;

    CalculationFamily f = prefixFamily("", "k:");
    QVERIFY(!registry.registerFamily(f));
    f.id = QStringLiteral("fa#m");
    QVERIFY(!registry.registerFamily(f));
    QVERIFY(!registry.contains("fa#m"));
    f.id = QStringLiteral("fam");
    f.instantiate = nullptr;
    QVERIFY(!registry.registerFamily(f));
    QVERIFY(!registry.registerSourceConversion(f));
    QVERIFY(!registry.contains("fam"));
}

void CalcRegistryTest::invalidFamilyInstanceDoesNotMatch()
{
    CalculationRegistry registry;

    // Instances are validated on first instantiation with the same rules as a
    // plain registration; an invalid instance is "family does not match".
    CalculationFamily bad;
    bad.id = QStringLiteral("bad");
    bad.instantiate = [](const DependencyKey &name) -> std::optional<CalculationDescriptor> {
        CalculationDescriptor d;
        d.compute = [](const EvaluationContext &) { return CalculationResult(); };
        d.outputs = {name};
        const QString key = name.attributeKey;
        if (key == QLatin1String("bad:hash"))
            d.id = QStringLiteral("has#hash");
        else if (key == QLatin1String("bad:emptykey"))
            d.id = QString();
        else if (key == QLatin1String("bad:nocompute")) {
            d.id = key;
            d.compute = nullptr;
        } else if (key == QLatin1String("bad:selfinput")) {
            d.id = key;
            d.inputs = {CalcInput::attribute(key)};
        } else if (key == QLatin1String("bad:source")) {
            d.id = key;
            d.inputs = {CalcInput::sourceMeasurement("S", "m")};
        } else if (key == QLatin1String("bad:wrongoutput")) {
            d.id = key;
            d.outputs = {DependencyKey::attribute(QStringLiteral("somethingElse"))};
        } else if (key == QLatin1String("bad:throws")) {
            throw std::runtime_error("instantiate failed");
        } else if (key == QLatin1String("bad:fine")) {
            d.id = key;
        } else {
            return std::nullopt;
        }
        return d;
    };
    QVERIFY(registry.registerFamily(bad));

    for (const char *name : {"bad:hash", "bad:emptykey", "bad:nocompute", "bad:selfinput", "bad:source",
                             "bad:wrongoutput", "bad:throws"}) {
        QVERIFY2(registry.candidatesFor(attr(name)).isEmpty(), name);
        QVERIFY2(!registry.hasCandidateFor(attr(name)), name);
    }
    QCOMPARE(ids(registry.candidatesFor(attr("bad:fine"))), QStringList({"bad#bad:fine"}));
    QCOMPARE(registry.memoizedInstanceCount("bad"), 1);
}

void CalcRegistryTest::familyInstanceIsMemoized()
{
    CalculationRegistry registry;
    QVERIFY(registry.registerFamily(pairFamily()));

    // Two names of the same instance: same instance id, same descriptor object.
    const QList<CalculationInstance> lo = registry.candidatesFor(attr("pair:lo"));
    const QList<CalculationInstance> hi = registry.candidatesFor(attr("pair:hi"));
    QCOMPARE(lo.size(), 1);
    QCOMPARE(hi.size(), 1);
    QCOMPARE(lo.first().instanceId, QStringLiteral("pairs#pair"));
    QCOMPARE(hi.first().instanceId, QStringLiteral("pairs#pair"));
    QCOMPARE(lo.first().descriptor.get(), hi.first().descriptor.get());
    QCOMPARE(lo.first().descriptor->outputs.size(), 2);
    QCOMPARE(registry.memoizedInstanceCount("pairs"), 1);

    // Asking again returns the memoized instance.
    QCOMPARE(registry.candidatesFor(attr("pair:lo")).first().descriptor.get(), lo.first().descriptor.get());
    QCOMPARE(registry.memoizedInstanceCount("pairs"), 1);
}

void CalcRegistryTest::unregisterFamilyDropsInstances()
{
    CalculationRegistry registry;
    QVERIFY(registry.registerFamily(prefixFamily("fam", "k:")));
    QCOMPARE(ids(registry.candidatesFor(attr("k:1"))), QStringList({"fam#k:1"}));
    QCOMPARE(ids(registry.candidatesFor(attr("k:2"))), QStringList({"fam#k:2"}));
    QCOMPARE(registry.memoizedInstanceCount("fam"), 2);

    QVERIFY(registry.unregister("fam"));
    QVERIFY(!registry.contains("fam"));
    QVERIFY(registry.candidatesFor(attr("k:1")).isEmpty());
    QCOMPARE(registry.memoizedInstanceCount("fam"), 0);

    // Registered again: a new family with an empty memo.
    QVERIFY(registry.registerFamily(prefixFamily("fam", "k:")));
    QCOMPARE(registry.memoizedInstanceCount("fam"), 0);
    QCOMPARE(ids(registry.candidatesFor(attr("k:1"))), QStringList({"fam#k:1"}));
    QCOMPARE(registry.memoizedInstanceCount("fam"), 1);
}

void CalcRegistryTest::hasCandidateFor()
{
    CalculationRegistry registry;
    Synthetic::registerSharedWorld(registry);

    QVERIFY(registry.hasCandidateFor(attr("X")));
    QVERIFY(registry.hasCandidateFor(attr("W")));
    QVERIFY(registry.hasCandidateFor(attr("neg:anything")));
    QVERIFY(registry.hasCandidateFor(measKey("S", "d")));
    QVERIFY(!registry.hasCandidateFor(measKey("S", "m")));
    QVERIFY(!registry.hasCandidateFor(attr("A")));
    QVERIFY(!registry.hasCandidateFor(attr("neg:")));
    QVERIFY(!registry.hasCandidateFor(measKey("neg:A", "")));

    QCOMPARE(ids(registry.candidatesFor(attr("X"))), QStringList({"sum", "fallbackX", "constX"}));
    QCOMPARE(ids(registry.candidatesFor(attr("W"))), QStringList({"triple", "wAlt"}));
    QCOMPARE(ids(registry.candidatesFor(attr("X2"))), QStringList({"P", "Q"}));
}

void CalcRegistryTest::instanceLookup()
{
    CalculationRegistry registry;
    Synthetic::registerSharedWorld(registry);

    const std::optional<CalculationInstance> triple = registry.instance("triple");
    QVERIFY(triple.has_value());
    QCOMPARE(triple->instanceId, QStringLiteral("triple"));
    QCOMPARE(triple->descriptor->outputs.size(), 3);

    QVERIFY(!registry.instance("nope").has_value());
    QVERIFY(!registry.instance("neg").has_value());                 // a family needs a name
    QVERIFY(!registry.instance("neg", attr("X")).has_value());      // not a name of the family
    const std::optional<CalculationInstance> negA = registry.instance("neg", attr("neg:A"));
    QVERIFY(negA.has_value());
    QCOMPARE(negA->instanceId, QStringLiteral("neg#neg:A"));
    QCOMPARE(negA->registrationId, QStringLiteral("neg"));
}

void CalcRegistryTest::sourceConversionsAreSeparate()
{
    CalculationRegistry registry;
    QVERIFY(!registry.hasSourceConversions());

    CalculationFamily conv;
    conv.id = QStringLiteral("conv");
    conv.instantiate = [](const DependencyKey &name) -> std::optional<CalculationDescriptor> {
        if (name.type != DependencyKey::Type::Measurement)
            return std::nullopt;
        const QString sensor = name.measurementKey.first;
        const QString meas = name.measurementKey.second;
        CalculationDescriptor d;
        d.id = sensor + QLatin1Char('/') + meas;
        d.inputs = {CalcInput::sourceMeasurement(sensor, meas), CalcInput::sourceUnit(sensor, meas)};
        d.outputs = {name};
        d.compute = [](const EvaluationContext &) { return CalculationResult(); };
        return d;
    };
    QVERIFY(registry.registerSourceConversion(conv));
    QVERIFY(registry.hasSourceConversions());

    // Source inputs are accepted here, and only here.
    const QList<CalculationInstance> list = registry.sourceConversionsFor("S", "m");
    QCOMPARE(ids(list), QStringList({"conv#S/m"}));
    QVERIFY(list.first().sourceConversion);

    // Conversions are not derived candidates.
    QVERIFY(registry.candidatesFor(measKey("S", "m")).isEmpty());
    QVERIFY(!registry.hasCandidateFor(measKey("S", "m")));

    // The same family registered as an ordinary family is rejected per instance.
    CalculationFamily asFamily = conv;
    asFamily.id = QStringLiteral("notAConversion");
    QVERIFY(registry.registerFamily(asFamily));
    QVERIFY(registry.candidatesFor(measKey("S", "m")).isEmpty());

    QVERIFY(registry.unregister("conv"));
    QVERIFY(!registry.hasSourceConversions());
    QVERIFY(registry.sourceConversionsFor("S", "m").isEmpty());
}

void CalcRegistryTest::localRegistryIsIsolated()
{
    // Reading the process-wide registry here is the point of this one test: a
    // private registry shares nothing with it, and the tests leave it empty.
    CalculationRegistry local;
    QVERIFY(&local != &CalculationRegistry::instance());
    QVERIFY(local.registerCalculation(simple("onlyLocal", "x")));
    QVERIFY(local.contains("onlyLocal"));
    QVERIFY(!CalculationRegistry::instance().contains("onlyLocal"));
    QVERIFY(!CalculationRegistry::instance().hasCandidateFor(attr("x")));

    CalculationRegistry other;
    QVERIFY(!other.contains("onlyLocal"));
    QVERIFY(other.registerCalculation(simple("onlyLocal", "x")));   // same id, no clash

    FakePreferenceProvider provider;
    local.setPreferenceProvider(&provider);
    QVERIFY(local.preferenceProvider() == &provider);
    QVERIFY(other.preferenceProvider() == nullptr);
    QVERIFY(CalculationRegistry::instance().preferenceProvider() == nullptr);
}

void CalcRegistryTest::enrolment()
{
    CalculationRegistry registry;
    FakeSessionState state;
    QCOMPARE(registry.enrolledEngineCount(), 0);
    {
        CalculationEngine a(&state, &registry);
        QCOMPARE(registry.enrolledEngineCount(), 1);
        {
            CalculationEngine b(&state, &registry);
            QCOMPARE(registry.enrolledEngineCount(), 2);
        }
        QCOMPARE(registry.enrolledEngineCount(), 1);
    }
    QCOMPARE(registry.enrolledEngineCount(), 0);
}

void CalcRegistryTest::registrationDuringEvaluationRejected()
{
#ifndef QT_NO_DEBUG
    QSKIP("asserts in debug builds; the release behavior is what is pinned here");
#else
    CalculationRegistry registry;
    FakeSessionState state;
    CalculationEngine engine(&state, &registry);

    bool registered = true;
    bool unregistered = true;
    CalculationDescriptor d = simple("meddler", "x");
    d.compute = [&](const EvaluationContext &) {
        registered = registry.registerCalculation(simple("late", "y"));
        unregistered = registry.unregister("meddler");
        return CalculationResult().setAttribute("x", 1);
    };
    QVERIFY(registry.registerCalculation(d));

    QCOMPARE(engine.attribute("x"), QVariant(1));
    QVERIFY(!registered);
    QVERIFY(!unregistered);
    QVERIFY(!registry.contains("late"));
    QVERIFY(registry.contains("meddler"));

    // Outside the evaluation the same calls succeed.
    QVERIFY(registry.registerCalculation(simple("late", "y")));
#endif
}

// ─────────────────────────────── registration-derived queries (logbook column cache)
// Self-contained block: static closures, declared preferences, observers.

void CalcRegistryTest::staticDependenciesClosure()
{
    CalculationRegistry registry;
    Synthetic::registerSharedWorld(registry);

    // Pure functions of the registrations: no engine, no session, no compute.
    FakeSessionState state;
    CalculationEngine engine(&state, &registry);

    // Z comes from the losing candidate wAlt, C from the losing candidate
    // fallbackX: every candidate is followed, not only the one that would win.
    const StaticDependencies w = registry.staticDependencies(attr("W"));
    QCOMPARE(w.names, QSet<DependencyKey>({attr("W"), attr("X"), attr("Z"), attr("A"), attr("B"), attr("C")}));
    QCOMPARE(w.preferences, QSet<QString>({QStringLiteral("p")}));

    // Memoized: the same answer again
    QCOMPARE(registry.staticDependencies(attr("W")).names, w.names);

    // A registry change drops the memo
    QVERIFY(registry.unregister(QStringLiteral("wAlt")));
    QCOMPARE(registry.staticDependencies(attr("W")).names,
             QSet<DependencyKey>({attr("W"), attr("X"), attr("A"), attr("B"), attr("C")}));

    // Terminates on the P / Q / R / S ring
    const StaticDependencies x2 = registry.staticDependencies(attr("X2"));
    QCOMPARE(x2.names, QSet<DependencyKey>({attr("X2"), attr("Y2")}));
    QVERIFY(x2.preferences.isEmpty());

    // A measurement, and through it an attribute chain
    QCOMPARE(registry.staticDependencies(measKey("S", "d")).names,
             QSet<DependencyKey>({measKey("S", "d"), measKey("S", "m"), attr("Y"),
                                  attr("X"), attr("A"), attr("B"), attr("C")}));

    // A family instance is instantiated from the name alone
    QCOMPARE(registry.staticDependencies(attr("neg:A")).names,
             QSet<DependencyKey>({attr("neg:A"), attr("A")}));

    // A name nothing produces: only itself
    QCOMPARE(registry.staticDependencies(attr("A")).names, QSet<DependencyKey>({attr("A")}));

    QCOMPARE(engine.totalRunCount(), 0);
    QCOMPARE(state.readCount(), 0);
}

void CalcRegistryTest::declaredPreferenceKeys()
{
    CalculationRegistry registry;
    QVERIFY(registry.declaredPreferenceKeys().isEmpty());

    Synthetic::registerSharedWorld(registry);
    QCOMPARE(registry.declaredPreferenceKeys(), QStringList({QStringLiteral("p")}));

    // Sorted and unique
    CalculationDescriptor extra = simple(QStringLiteral("extra"), QStringLiteral("E"));
    extra.inputs = {CalcInput::preference(QStringLiteral("p")), CalcInput::preference(QStringLiteral("a/first"))};
    QVERIFY(registry.registerCalculation(extra));
    QCOMPARE(registry.declaredPreferenceKeys(), QStringList({QStringLiteral("a/first"), QStringLiteral("p")}));

    QVERIFY(registry.unregister(QStringLiteral("triple")));
    QVERIFY(registry.unregister(QStringLiteral("extra")));
    QVERIFY(registry.declaredPreferenceKeys().isEmpty());
}

void CalcRegistryTest::observersFire()
{
    CalculationRegistry registry;
    int calls = 0;
    int otherCalls = 0;
    const int token = registry.addObserver([&calls]() { ++calls; });
    const int otherToken = registry.addObserver([&otherCalls]() { ++otherCalls; });
    QVERIFY(token != otherToken);

    // One call per successful register / unregister, of every kind
    QVERIFY(registry.registerCalculation(simple(QStringLiteral("a"), QStringLiteral("A"))));
    QCOMPARE(calls, 1);
    QVERIFY(registry.registerFamily(prefixFamily(QStringLiteral("fam"), QStringLiteral("f:"))));
    QCOMPARE(calls, 2);
    QVERIFY(registry.registerSourceConversion(prefixFamily(QStringLiteral("conv"), QStringLiteral("c:"))));
    QCOMPARE(calls, 3);
    QVERIFY(registry.unregister(QStringLiteral("fam")));
    QCOMPARE(calls, 4);

    // None for a rejected registration or a failed unregister
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("already registered")));
    QVERIFY(!registry.registerCalculation(simple(QStringLiteral("a"), QStringLiteral("B"))));
    QVERIFY(!registry.unregister(QStringLiteral("no-such-id")));
    QCOMPARE(calls, 4);

    // None after removeObserver; the other observer keeps firing
    registry.removeObserver(token);
    QVERIFY(registry.unregister(QStringLiteral("a")));
    QCOMPARE(calls, 4);
    QCOMPARE(otherCalls, 5);

    registry.removeObserver(otherToken);
    registry.removeObserver(12345);     // unknown tokens are ignored
}

// ---- CalculationDescriptor::allowSourceInputs (Python plugin host) ---------

void CalcRegistryTest::sourceInputsOptIn()
{
    CalculationRegistry registry;

    CalculationDescriptor d = simple(QStringLiteral("reader"), QStringLiteral("out"));
    d.inputs = {CalcInput::sourceMeasurement("S", "m"), CalcInput::sourceUnit("S", "m")};

    // Default: a plain calculation may not read the source layer.
    QVERIFY(!d.allowSourceInputs);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("may read the source layer")));
    QVERIFY(!registry.registerCalculation(d));
    QVERIFY(!registry.contains(QStringLiteral("reader")));

    // With the opt-in the same descriptor is accepted, as an ordinary candidate.
    d.allowSourceInputs = true;
    QVERIFY(registry.registerCalculation(d));
    QVERIFY(registry.hasCandidateFor(attr("out")));
    QVERIFY(registry.sourceConversionsFor(QStringLiteral("S"), QStringLiteral("m")).isEmpty());

    // The flag opens nothing else: an own output as an input is still refused.
    CalculationDescriptor own = simple(QStringLiteral("own"), QStringLiteral("own"));
    own.allowSourceInputs = true;
    own.inputs = {CalcInput::attribute("own")};
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("its own output")));
    QVERIFY(!registry.registerCalculation(own));
}

// ---- staticDependencies() and the source-input opt-in compose --------------

// A logbook column fed by a source-reading plugin must refresh when its source
// column is merged: staticDependencies has to see opt-in source inputs as the
// public measurement name whose source layer they read.
void CalcRegistryTest::staticDependenciesCoverOptInSourceInputs()
{
    CalculationRegistry registry;

    CalculationDescriptor d = simple(QStringLiteral("srcProbe"), QStringLiteral("SRC0"));
    d.allowSourceInputs = true;
    d.inputs = {CalcInput::sourceMeasurement("S", "m"), CalcInput::sourceUnit("S", "m"),
                CalcInput::attribute("A")};
    QVERIFY(registry.registerCalculation(d));

    const StaticDependencies deps = registry.staticDependencies(attr("SRC0"));
    QCOMPARE(deps.names, QSet<DependencyKey>({attr("SRC0"), measKey("S", "m"), attr("A")}));
    QVERIFY(deps.preferences.isEmpty());
}

FLYSIGHT_TEST_MAIN(CalcRegistryTest)
#include "tst_calcregistry.moc"
