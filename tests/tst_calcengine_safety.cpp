// CalculationEngine safety (acceptance 12): nested evaluation with per-scope
// dependency recording, cycle detection (read-order independent for a single
// ring, including a ring closed through request()),
// exception safety, and the re-entrancy guards. After every scenario the cached
// answers are compared with a fresh evaluation.

#include <optional>
#include <stdexcept>

#include <QSet>
#include <QtTest>

#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "fakesessionstate.h"
#include "testmain.h"

using namespace FlySight;
using namespace FlySightTest;
using Synthetic::attr;
using Synthetic::measKey;

namespace {

using Names = QSet<DependencyKey>;

struct World {
    CalculationRegistry registry;
    FakePreferenceProvider prefs;
    FakeSessionState state;
    CalculationEngine engine;

    explicit World(bool sharedWorld = true)
        : engine(&state, &registry)
    {
        registry.setPreferenceProvider(&prefs);
        if (sharedWorld)
            Synthetic::registerSharedWorld(registry);
    }
};

QList<DependencyKey> sharedNames()
{
    return {attr("A"), attr("X"), attr("Y"), attr("Z"), attr("W"), attr("X2"), attr("Y2"),
            attr("neg:A"), attr("neg:X"), measKey("S", "d"), measKey("S", "m")};
}

// thrower: input A; outputs T1, T2. Sets T1 on its bundle and then throws while
// A < 10; with A >= 10 it returns T1 = A, T2 = A + 1.
CalculationDescriptor thrower(bool standardException)
{
    CalculationDescriptor d;
    d.id = QStringLiteral("thrower");
    d.inputs = {CalcInput::attribute("A")};
    d.outputs = {attr("T1"), attr("T2")};
    d.compute = [standardException](const EvaluationContext &ctx) {
        const int a = ctx.attribute("A").toInt();
        CalculationResult r;
        r.setAttribute("T1", a);            // already set when the exception leaves
        if (a < 10) {
            if (standardException)
                throw std::runtime_error("synthetic failure");
            throw 42;
        }
        r.setAttribute("T2", a + 1);
        return r;
    };
    return d;
}

// outer: input T1; output O = T1 + 1.
CalculationDescriptor outer()
{
    CalculationDescriptor d;
    d.id = QStringLiteral("outer");
    d.inputs = {CalcInput::attribute("T1")};
    d.outputs = {attr("O")};
    d.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute("O", ctx.attribute("T1").toInt() + 1);
    };
    return d;
}

// explicitE: Explicit policy; input EY; output EX = EY + 1.
CalculationDescriptor explicitE()
{
    CalculationDescriptor d;
    d.id = QStringLiteral("explicitE");
    d.policy = EvaluationPolicy::Explicit;
    d.inputs = {CalcInput::attribute("EY")};
    d.outputs = {attr("EX")};
    d.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute("EX", ctx.attribute("EY").toInt() + 1);
    };
    return d;
}

// feedsE: input EX; output EY = EX * 2. Together with explicitE it closes a
// ring through the explicit calculation's own output.
CalculationDescriptor feedsE()
{
    CalculationDescriptor d;
    d.id = QStringLiteral("feedsE");
    d.inputs = {CalcInput::attribute("EX")};
    d.outputs = {attr("EY")};
    d.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute("EY", ctx.attribute("EX").toInt() * 2);
    };
    return d;
}

// A state whose reads can be made to throw: an exception that does not come
// from a compute function.
class ThrowingState : public FakeSessionState {
public:
    bool throwing = false;
    QVariant storedAttribute(const QString &key) const override
    {
        if (throwing)
            throw std::runtime_error("state failure");
        return FakeSessionState::storedAttribute(key);
    }
};

} // namespace

class CalcEngineSafetyTest : public QObject {
    Q_OBJECT

private slots:
    void nestedChainFromOneRead();
    void cycleFallsBackIndependentOfReadOrder_data();
    void cycleFallsBackIndependentOfReadOrder();
    void pureCycleTerminates();
    void requestThroughOwnOutputIsCycle_data();
    void requestThroughOwnOutputIsCycle();
    void exceptionLeavesNoPartialResult_data();
    void exceptionLeavesNoPartialResult();
    void innerExceptionKeepsUnrelatedResults();
    void foreignExceptionPublishesNothing();
    void readFromComputeIsRejected();
    void notificationDuringEvaluationIsDeferred();
};

// Acceptance 12: nested evaluation records the right dependencies per scope.
void CalcEngineSafetyTest::nestedChainFromOneRead()
{
    World w;
    w.state.setAttribute("A", 1);
    w.state.setAttribute("B", 2);
    w.prefs.set("p", 5);
    w.state.setMeasurement("S", "m", {1.0, 2.0, 3.0});

    // One read drives meas -> triple -> sum, three scopes deep.
    QCOMPARE(w.engine.measurement("S", "d"), (QVector<double>{9.0, 10.0, 11.0}));
    QCOMPARE(w.engine.scopeDepth(), 0);
    QCOMPARE(w.engine.runCount("sum"), 1);
    QCOMPARE(w.engine.runCount("triple"), 1);
    QCOMPARE(w.engine.runCount("meas"), 1);
    QCOMPARE(w.engine.totalRunCount(), 3);
    QCOMPARE(w.engine.cycleCount(), 0);

    // Each scope recorded its own reads and nothing from the scopes nested in it.
    QCOMPARE(w.engine.dependenciesOf(GraphNode::result("meas")),
             QSet<GraphNode>({GraphNode::resolution(measKey("S", "m")), GraphNode::resolution(attr("Y"))}));
    QCOMPARE(w.engine.dependenciesOf(GraphNode::result("triple")),
             QSet<GraphNode>({GraphNode::resolution(attr("X")), GraphNode::preference("p")}));
    QCOMPARE(w.engine.dependenciesOf(GraphNode::result("sum")),
             QSet<GraphNode>({GraphNode::resolution(attr("A")), GraphNode::resolution(attr("B"))}));
    QCOMPARE(w.engine.dependenciesOf(GraphNode::resolution(attr("X"))),
             QSet<GraphNode>({GraphNode::storedAttribute("X"), GraphNode::result("sum")}));

    QVERIFY(w.engine.verifyAgainstFresh(sharedNames()).isEmpty());
    QCOMPARE(w.engine.scopeDepth(), 0);
}

void CalcEngineSafetyTest::cycleFallsBackIndependentOfReadOrder_data()
{
    QTest::addColumn<bool>("xFirst");
    QTest::newRow("X2 then Y2") << true;
    QTest::newRow("Y2 then X2") << false;
}

// Acceptance 12: a cycle is reported and unwinds cleanly. X2 has candidates
// [P(Y2), Q], Y2 has [R(X2), S]: P and R are on a ring, so both are unavailable
// and the names fall back - whichever is read first.
void CalcEngineSafetyTest::cycleFallsBackIndependentOfReadOrder()
{
    QFETCH(bool, xFirst);
    World w;

    if (xFirst) {
        QCOMPARE(w.engine.attribute("X2"), QVariant(100));
        QCOMPARE(w.engine.attribute("Y2"), QVariant(200));
    } else {
        QCOMPARE(w.engine.attribute("Y2"), QVariant(200));
        QCOMPARE(w.engine.attribute("X2"), QVariant(100));
    }
    QVERIFY(w.engine.cycleCount() >= 1);
    QVERIFY(w.engine.lastCyclePath().size() >= 3);
    QCOMPARE(w.engine.lastCyclePath().first(), w.engine.lastCyclePath().last());
    QCOMPARE(w.engine.runCount("P"), 0);
    QCOMPARE(w.engine.runCount("R"), 0);
    QCOMPARE(w.engine.resultStatus("P"), std::optional<ResultStatus>(ResultStatus::Cycle));
    QCOMPARE(w.engine.resultStatus("R"), std::optional<ResultStatus>(ResultStatus::Cycle));
    QCOMPARE(w.engine.scopeDepth(), 0);
    QVERIFY(w.engine.verifyAgainstFresh(sharedNames()).isEmpty());

    // Every edge of the ring was recorded, so breaking it re-resolves the ring.
    const Names names = w.state.setAttribute(w.engine, "Y2", 5);
    QVERIFY(names.contains(attr("X2")));
    QVERIFY(names.contains(attr("Y2")));
    QCOMPARE(w.engine.attribute("X2"), QVariant(6));
    QCOMPARE(w.engine.attribute("Y2"), QVariant(5));
    QCOMPARE(w.engine.runCount("P"), 1);
    QCOMPARE(w.engine.runCount("R"), 0);
    QVERIFY(w.engine.verifyAgainstFresh(sharedNames()).isEmpty());

    // And closing it again falls back again.
    QVERIFY(w.state.removeAttribute(w.engine, "Y2").contains(attr("X2")));
    QCOMPARE(w.engine.attribute("X2"), QVariant(100));
    QCOMPARE(w.engine.attribute("Y2"), QVariant(200));
    QVERIFY(w.engine.verifyAgainstFresh(sharedNames()).isEmpty());
    QCOMPARE(w.engine.scopeDepth(), 0);
}

void CalcEngineSafetyTest::pureCycleTerminates()
{
    World w(false);
    QVERIFY(w.registry.registerCalculation(Synthetic::P()));
    QVERIFY(w.registry.registerCalculation(Synthetic::R()));

    QVERIFY(!w.engine.attribute("X2").isValid());
    QVERIFY(!w.engine.attribute("Y2").isValid());
    QVERIFY(w.engine.cycleCount() >= 1);
    QCOMPARE(w.engine.scopeDepth(), 0);
    QCOMPARE(w.engine.totalRunCount(), 0);
    QCOMPARE(w.engine.cachedState(attr("X2")), CalculationEngine::CachedState::Unavailable);
    QCOMPARE(w.engine.cachedState(attr("Y2")), CalculationEngine::CachedState::Unavailable);
    QVERIFY(w.engine.verifyAgainstFresh({attr("X2"), attr("Y2")}).isEmpty());

    // Cached: reading again neither recurses nor reports again.
    const int cycles = w.engine.cycleCount();
    QVERIFY(!w.engine.attribute("X2").isValid());
    QCOMPARE(w.engine.cycleCount(), cycles);

    QVERIFY(w.state.setAttribute(w.engine, "Y2", 5).contains(attr("X2")));
    QCOMPARE(w.engine.attribute("X2"), QVariant(6));
    QCOMPARE(w.engine.attribute("Y2"), QVariant(5));
    QVERIFY(w.engine.verifyAgainstFresh({attr("X2"), attr("Y2")}).isEmpty());
    QCOMPARE(w.engine.scopeDepth(), 0);
}

void CalcEngineSafetyTest::requestThroughOwnOutputIsCycle_data()
{
    QTest::addColumn<bool>("readFirst");
    QTest::newRow("nothing cached") << false;
    QTest::newRow("not-requested answers cached") << true;
}

// Acceptance 12: an explicit calculation whose input is produced from its own
// output is a cycle, and request() says so. The cached "not requested" answer
// must not hide the ring: the nested lookup has to reach the stack check, and
// no input may be served from an answer that was derived from "not requested".
void CalcEngineSafetyTest::requestThroughOwnOutputIsCycle()
{
    QFETCH(bool, readFirst);
    World w(false);
    QVERIFY(w.registry.registerCalculation(explicitE()));
    QVERIFY(w.registry.registerCalculation(feedsE()));
    const QList<DependencyKey> names = {attr("EX"), attr("EY")};

    if (readFirst) {
        // Reads never start the explicit calculation, so there is no ring yet:
        // EX is "not requested", and EY is missing that input.
        QVERIFY(!w.engine.attribute("EX").isValid());
        QVERIFY(!w.engine.attribute("EY").isValid());
        QCOMPARE(w.engine.cycleCount(), 0);
        QCOMPARE(w.engine.resultStatus("explicitE"), std::optional<ResultStatus>(ResultStatus::NotRequested));
        QCOMPARE(w.engine.resultStatus("feedsE"), std::optional<ResultStatus>(ResultStatus::MissingInput));
    }

    const CalculationEngine::RequestOutcome outcome = w.engine.request("explicitE");
    QVERIFY(outcome.found);
    QCOMPARE(outcome.status, ResultStatus::Cycle);
    QCOMPARE(outcome.invalidated, Names({attr("EX"), attr("EY")}));
    QCOMPARE(w.engine.cycleCount(), 1);
    QCOMPARE(w.engine.lastCyclePath(),
             QList<GraphNode>({GraphNode::result("explicitE"), GraphNode::resolution(attr("EY")),
                               GraphNode::result("feedsE"), GraphNode::resolution(attr("EX")),
                               GraphNode::result("explicitE")}));
    QCOMPARE(w.engine.scopeDepth(), 0);
    QCOMPARE(w.engine.totalRunCount(), 0);          // neither compute function ran

    // Nothing was published for either output, and the ring is cached as such.
    QCOMPARE(w.engine.resultStatus("explicitE"), std::optional<ResultStatus>(ResultStatus::Cycle));
    QVERIFY(!w.engine.attribute("EX").isValid());
    QVERIFY(!w.engine.attribute("EY").isValid());
    QCOMPARE(w.engine.cachedState(attr("EX")), CalculationEngine::CachedState::Unavailable);
    QCOMPARE(w.engine.cachedState(attr("EY")), CalculationEngine::CachedState::Unavailable);
    QCOMPARE(w.engine.totalRunCount(), 0);
    QVERIFY(w.engine.verifyAgainstFresh(names).isEmpty());
    QCOMPARE(w.engine.scopeDepth(), 0);

    // Asking again does not re-evaluate: the cycle answer is a valid result.
    const int cycles = w.engine.cycleCount();
    QCOMPARE(w.engine.request("explicitE").status, ResultStatus::Cycle);
    QCOMPARE(w.engine.cycleCount(), cycles);

    // Storing EY breaks the ring. The explicit calculation reverts to "not
    // requested" because its input changed; requested again, it runs once.
    QVERIFY(w.state.setAttribute(w.engine, "EY", 5).contains(attr("EX")));
    QVERIFY(!w.engine.attribute("EX").isValid());
    QCOMPARE(w.engine.request("explicitE").status, ResultStatus::Ok);
    QCOMPARE(w.engine.attribute("EX"), QVariant(6));
    QCOMPARE(w.engine.attribute("EY"), QVariant(5));
    QCOMPARE(w.engine.runCount("explicitE"), 1);
    QCOMPARE(w.engine.runCount("feedsE"), 0);
    QCOMPARE(w.engine.cycleCount(), cycles);
    QVERIFY(w.engine.verifyAgainstFresh(names).isEmpty());
    QCOMPARE(w.engine.scopeDepth(), 0);
}

void CalcEngineSafetyTest::exceptionLeavesNoPartialResult_data()
{
    QTest::addColumn<bool>("standardException");
    QTest::newRow("std::runtime_error") << true;
    QTest::newRow("throw 42") << false;
}

// Acceptance 12: an exception leaves no partial result and no corrupted scope.
void CalcEngineSafetyTest::exceptionLeavesNoPartialResult()
{
    QFETCH(bool, standardException);
    World w(false);
    QVERIFY(w.registry.registerCalculation(thrower(standardException)));
    QVERIFY(w.registry.registerCalculation(outer()));
    w.state.setAttribute("A", 1);

    // The read that nests outer -> thrower returns normally.
    QVERIFY(!w.engine.attribute("O").isValid());
    QCOMPARE(w.engine.scopeDepth(), 0);
    QVERIFY(!w.engine.attribute("T1").isValid());   // was set on the bundle before the throw
    QVERIFY(!w.engine.attribute("T2").isValid());
    QCOMPARE(w.engine.resultStatus("thrower"), std::optional<ResultStatus>(ResultStatus::Failed));
    QCOMPARE(w.engine.resultStatus("outer"), std::optional<ResultStatus>(ResultStatus::MissingInput));
    QCOMPARE(w.engine.runCount("outer"), 0);

    // Negatively cached with its input dependencies: not retried by reads.
    for (int i = 0; i < 3; ++i) {
        QVERIFY(!w.engine.attribute("T1").isValid());
        QVERIFY(!w.engine.attribute("O").isValid());
    }
    QCOMPARE(w.engine.runCount("thrower"), 1);
    QCOMPARE(w.engine.dependenciesOf(GraphNode::result("thrower")),
             QSet<GraphNode>({GraphNode::resolution(attr("A"))}));
    QVERIFY(w.engine.verifyAgainstFresh({attr("O"), attr("T1"), attr("T2")}).isEmpty());

    // Its input changes: exactly one more run (which fails again).
    QVERIFY(w.state.setAttribute(w.engine, "A", 2).contains(attr("O")));
    QVERIFY(!w.engine.attribute("O").isValid());
    QVERIFY(!w.engine.attribute("T2").isValid());
    QCOMPARE(w.engine.runCount("thrower"), 2);

    // And once it stops throwing, everything appears.
    w.state.setAttribute(w.engine, "A", 20);
    QCOMPARE(w.engine.attribute("O"), QVariant(21));
    QCOMPARE(w.engine.attribute("T1"), QVariant(20));
    QCOMPARE(w.engine.attribute("T2"), QVariant(21));
    QCOMPARE(w.engine.runCount("thrower"), 3);
    QCOMPARE(w.engine.runCount("outer"), 1);
    QCOMPARE(w.engine.scopeDepth(), 0);
    QVERIFY(w.engine.verifyAgainstFresh({attr("O"), attr("T1"), attr("T2")}).isEmpty());
}

void CalcEngineSafetyTest::innerExceptionKeepsUnrelatedResults()
{
    World w;
    QVERIFY(w.registry.registerCalculation(thrower(true)));
    QVERIFY(w.registry.registerCalculation(outer()));
    w.state.setAttribute("A", 1);
    w.state.setAttribute("B", 2);
    w.prefs.set("p", 5);

    QCOMPARE(w.engine.attribute("X"), QVariant(3));
    QCOMPARE(w.engine.attribute("Y"), QVariant(8));
    const int runs = w.engine.totalRunCount();
    const int edges = w.engine.edgeCount();

    QVERIFY(!w.engine.attribute("O").isValid());
    QCOMPARE(w.engine.scopeDepth(), 0);
    QCOMPARE(w.engine.totalRunCount(), runs + 1);   // the thrower, once

    // Previously cached results are intact and still served from the cache.
    QCOMPARE(w.engine.cachedState(attr("X")), CalculationEngine::CachedState::Available);
    QCOMPARE(w.engine.cachedState(attr("Y")), CalculationEngine::CachedState::Available);
    QCOMPARE(w.engine.attribute("X"), QVariant(3));
    QCOMPARE(w.engine.attribute("Y"), QVariant(8));
    QCOMPARE(w.engine.totalRunCount(), runs + 1);
    QVERIFY(w.engine.edgeCount() > edges);          // the failure has dependencies too

    QList<DependencyKey> names = sharedNames();
    names << attr("O") << attr("T1") << attr("T2");
    QVERIFY(w.engine.verifyAgainstFresh(names).isEmpty());
    QCOMPARE(w.engine.scopeDepth(), 0);
}

// An exception that does not come from a compute function propagates to the
// caller, but the scopes are popped and nothing is published for the nodes
// that were open.
void CalcEngineSafetyTest::foreignExceptionPublishesNothing()
{
    CalculationRegistry registry;
    Synthetic::registerSharedWorld(registry);
    ThrowingState state;
    state.setAttribute("A", 1);
    state.setAttribute("B", 2);
    CalculationEngine engine(&state, &registry);

    state.throwing = true;
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, engine.attribute("X"));
    QCOMPARE(engine.scopeDepth(), 0);
    QCOMPARE(engine.cachedNodeCount(), 0);
    QCOMPARE(engine.edgeCount(), 0);
    QCOMPARE(engine.totalRunCount(), 0);

    // The registry is not left believing an evaluation is in progress.
    QVERIFY(registry.unregister("wAlt"));
    QVERIFY(registry.registerCalculation(Synthetic::wAlt()));

    state.throwing = false;
    QCOMPARE(engine.attribute("X"), QVariant(3));
    QCOMPARE(engine.scopeDepth(), 0);
}

void CalcEngineSafetyTest::readFromComputeIsRejected()
{
#ifndef QT_NO_DEBUG
    QSKIP("asserts in debug builds; the release behavior is what is pinned here");
#else
    World w(false);
    CalculationEngine *engine = &w.engine;
    QVariant seen = 1;
    bool requestFound = true;

    CalculationDescriptor reentrant;
    reentrant.id = QStringLiteral("reentrant");
    reentrant.outputs = {attr("RE")};
    reentrant.compute = [engine, &seen, &requestFound](const EvaluationContext &) {
        seen = engine->attribute("A");                  // a bug in the calculation
        requestFound = engine->request("reentrant").found;
        return CalculationResult().setAttribute("RE", 1);
    };
    QVERIFY(w.registry.registerCalculation(reentrant));
    w.state.setAttribute("A", 1);

    QCOMPARE(w.engine.attribute("RE"), QVariant(1));
    QVERIFY(!seen.isValid());                           // reported unavailable, not resolved
    QVERIFY(!requestFound);
    QCOMPARE(w.engine.cachedState(attr("A")), CalculationEngine::CachedState::NotCached);
    QCOMPARE(w.engine.scopeDepth(), 0);
#endif
}

void CalcEngineSafetyTest::notificationDuringEvaluationIsDeferred()
{
#ifndef QT_NO_DEBUG
    QSKIP("asserts in debug builds; the release behavior is what is pinned here");
#else
    World w(false);
    QVERIFY(w.registry.registerCalculation(Synthetic::sum()));
    w.state.setAttribute("A", 1);
    w.state.setAttribute("B", 2);
    QCOMPARE(w.engine.attribute("X"), QVariant(3));

    QList<Names> broadcasts;
    w.engine.setInvalidationListener([&broadcasts](const Names &n) { broadcasts.append(n); });

    CalculationEngine *engine = &w.engine;
    Names returned;
    CalculationDescriptor meddler;
    meddler.id = QStringLiteral("meddler");
    meddler.outputs = {attr("M")};
    meddler.compute = [engine, &returned](const EvaluationContext &) {
        returned = engine->attributeChanged("A");       // a bug in the calculation
        return CalculationResult().setAttribute("M", 1);
    };
    QVERIFY(w.registry.registerCalculation(meddler));

    // Nothing is invalidated in the middle of the evaluation; the seeds are
    // applied once it has unwound and the names go to the listener.
    QCOMPARE(w.engine.attribute("M"), QVariant(1));
    QCOMPARE(returned, Names({attr("A")}));
    QCOMPARE(broadcasts, QList<Names>({Names({attr("A"), attr("X")})}));
    QCOMPARE(w.engine.cachedState(attr("X")), CalculationEngine::CachedState::NotCached);
    QCOMPARE(w.engine.cachedState(attr("M")), CalculationEngine::CachedState::Available);
    QCOMPARE(w.engine.scopeDepth(), 0);
#endif
}

FLYSIGHT_TEST_MAIN(CalcEngineSafetyTest)
#include "tst_calcengine_safety.moc"
