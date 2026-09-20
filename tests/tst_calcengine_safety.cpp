// CalculationEngine safety (acceptance 12): nested evaluation with per-scope
// dependency recording, cycle detection (read-order independent for single and
// for overlapping rings, including a ring closed through request()),
// exception safety, and the re-entrancy guards. After every scenario the cached
// answers are compared with a fresh evaluation.

#include <algorithm>
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

// constEY: output EY = 7. Registered after feedsE it is the fallback for EY.
CalculationDescriptor constEY()
{
    CalculationDescriptor d;
    d.id = QStringLiteral("constEY");
    d.outputs = {attr("EY")};
    d.compute = [](const EvaluationContext &) { return CalculationResult().setAttribute("EY", 7); };
    return d;
}

// One attribute output = one attribute input + add.
CalculationDescriptor plus(const char *id, const char *input, const char *output, int add)
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
    void overlappingRingsIndependentOfReadOrder_data();
    void overlappingRingsIndependentOfReadOrder();
    void overlappingRingsFollowEdits();
    void provisionalResultsAreNotCached();
    void ringBeneathAcyclicAncestorIsCached();
    void requestThroughOwnOutputIsCycle_data();
    void requestThroughOwnOutputIsCycle();
    void requestOnRingWithFallback_data();
    void requestOnRingWithFallback();
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

void CalcEngineSafetyTest::overlappingRingsIndependentOfReadOrder_data()
{
    QTest::addColumn<QStringList>("keys");
    QTest::addColumn<QVariantList>("expected");     // invalid = unavailable

    // See Synthetic::registerTangleWorld for the topologies and the reasoning
    // behind each literal.
    QTest::newRow("second candidate on a ring")
        << QStringList({"OX", "OY"}) << QVariantList({100, QVariant()});
    QTest::newRow("ring entered from two names, fallback outside")
        << QStringList({"E1", "E2", "OX", "OY"}) << QVariantList({101, 7, 100, QVariant()});
    QTest::newRow("two rings through one calculation")
        << QStringList({"A1", "B1", "C1"}) << QVariantList({1, 51, 50});
    QTest::newRow("three-name tangle")
        << QStringList({"K1", "K2", "K3"}) << QVariantList({10, 20, 31});
}

// Acceptance 10 / 12: with overlapping rings, which calculations are on the
// stack when a ring closes depends on where the read started. The answers must
// not: every permutation of the read order gives the same literal values, the
// first read (nothing cached) as well as the later ones (served from, or next
// to, what the earlier reads cached), and all of them equal a fresh evaluation.
void CalcEngineSafetyTest::overlappingRingsIndependentOfReadOrder()
{
    QFETCH(QStringList, keys);
    QFETCH(QVariantList, expected);
    QCOMPARE(keys.size(), expected.size());

    QList<int> order;
    for (int i = 0; i < int(keys.size()); ++i)
        order.append(i);

    int permutations = 0;
    do {
        QByteArray where = "order";
        for (int i : std::as_const(order))
            where += ' ' + keys.at(i).toLatin1();

        World w(false);
        Synthetic::registerTangleWorld(w.registry);

        for (int i : std::as_const(order))
            QVERIFY2(w.engine.attribute(keys.at(i)) == expected.at(i),
                     (where + ": first read of " + keys.at(i).toLatin1()).constData());
        QVERIFY2(w.engine.cycleCount() >= 1, where.constData());

        // Again, now that every name has been read at the top level.
        for (int i = 0; i < int(keys.size()); ++i)
            QVERIFY2(w.engine.attribute(keys.at(i)) == expected.at(i),
                     (where + ": second read of " + keys.at(i).toLatin1()).constData());

        const QList<DependencyKey> mismatch = w.engine.verifyAgainstFresh(Synthetic::tangleNames());
        QVERIFY2(mismatch.isEmpty(),
                 (where + ": differs from fresh: "
                  + (mismatch.isEmpty() ? QByteArray() : describe(mismatch.first()).toUtf8())).constData());
        QCOMPARE(w.engine.scopeDepth(), 0);
        ++permutations;
    } while (std::next_permutation(order.begin(), order.end()));

    QVERIFY(permutations >= 2);
}

// Breaking and closing the rings by storing a value re-resolves everything
// that was shaped by them, cached or provisional, from either side.
void CalcEngineSafetyTest::overlappingRingsFollowEdits()
{
    for (const bool yFirst : {false, true}) {
        World w(false);
        Synthetic::registerTangleWorld(w.registry);
        const QList<DependencyKey> names = Synthetic::tangleNames();

        if (yFirst)
            QVERIFY(!w.engine.attribute("OY").isValid());
        QCOMPARE(w.engine.attribute("E2"), QVariant(7));
        QCOMPARE(w.engine.attribute("OX"), QVariant(100));
        QVERIFY(w.engine.verifyAgainstFresh(names).isEmpty());

        // A stored OY cuts both rings: oP runs, and eB sees OY.
        Names dropped = w.state.setAttribute(w.engine, "OY", 5);
        QVERIFY(dropped.contains(attr("OX")));
        QVERIFY(dropped.contains(attr("E2")));
        w.engine.resetRunCounts();
        QCOMPARE(w.engine.attribute("OX"), QVariant(6));
        QCOMPARE(w.engine.attribute("E2"), QVariant(6));
        QCOMPARE(w.engine.attribute("E1"), QVariant(7));
        QCOMPARE(w.engine.attribute("OY"), QVariant(5));
        QCOMPARE(w.engine.runCount("oP"), 1);       // no ring left: nothing is provisional
        QCOMPARE(w.engine.runCount("eA"), 1);
        QCOMPARE(w.engine.runCount("eB"), 1);
        QCOMPARE(w.engine.totalRunCount(), 3);
        QVERIFY(w.engine.verifyAgainstFresh(names).isEmpty());

        // Removing it closes them again.
        dropped = w.state.removeAttribute(w.engine, "OY");
        QVERIFY(dropped.contains(attr("OX")));
        QVERIFY(dropped.contains(attr("E1")));
        QVERIFY(dropped.contains(attr("E2")));
        if (!yFirst)
            QCOMPARE(w.engine.attribute("OX"), QVariant(100));
        QVERIFY(!w.engine.attribute("OY").isValid());
        QCOMPARE(w.engine.attribute("OX"), QVariant(100));
        QCOMPARE(w.engine.attribute("E1"), QVariant(101));
        QCOMPARE(w.engine.attribute("E2"), QVariant(7));
        QVERIFY(w.engine.verifyAgainstFresh(names).isEmpty());

        // A registry change reaches an answer through a provisional candidate:
        // without oQ, OX has nothing to fall back to.
        QVERIFY(w.registry.unregister("oQ"));
        QVERIFY(!w.engine.attribute("OX").isValid());
        QVERIFY(!w.engine.attribute("E1").isValid());
        QVERIFY(w.engine.verifyAgainstFresh(names).isEmpty());
        QCOMPARE(w.engine.scopeDepth(), 0);
    }
}

// What a read caches when it crosses a ring: the root and everything that did
// not depend on the entry point; never a verdict that did. Inspection reports
// the provisional verdicts without computing.
void CalcEngineSafetyTest::provisionalResultsAreNotCached()
{
    using CachedState = CalculationEngine::CachedState;
    World w(false);
    Synthetic::registerTangleWorld(w.registry);

    QCOMPARE(w.engine.attribute("OX"), QVariant(100));

    // OY was unavailable BENEATH OX (oR and oS both met OX on the stack); that
    // is not the answer for OY itself, so nothing was cached for it. oQ met no ring.
    QCOMPARE(w.engine.cachedState(attr("OX")), CachedState::Available);
    QCOMPARE(w.engine.cachedState(attr("OY")), CachedState::NotCached);
    QCOMPARE(w.engine.cachedNodeCount(), 2);        // Resolution(OX), Result(oQ)
    QCOMPARE(w.engine.resultStatus("oQ"), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(w.engine.resultStatus("oP"), std::optional<ResultStatus>(ResultStatus::Cycle));
    QCOMPARE(w.engine.resultStatus("oR"), std::optional<ResultStatus>(ResultStatus::Cycle));
    QCOMPARE(w.engine.resultStatus("oS"), std::optional<ResultStatus>(ResultStatus::Cycle));
    QCOMPARE(w.engine.resultStatus("eA"), std::optional<ResultStatus>());

    // The cached answer depends on everything the provisional results looked at.
    const QSet<GraphNode> deps = w.engine.dependenciesOf(GraphNode::resolution(attr("OX")));
    QVERIFY(deps.contains(GraphNode::storedAttribute("OX")));
    QVERIFY(deps.contains(GraphNode::storedAttribute("OY")));
    QVERIFY(deps.contains(GraphNode::resolution(attr("OY"))));
    QVERIFY(deps.contains(GraphNode::result("oP")));
    QVERIFY(deps.contains(GraphNode::result("oR")));
    QVERIFY(deps.contains(GraphNode::result("oS")));
    QVERIFY(deps.contains(GraphNode::result("oQ")));
    QVERIFY(!deps.contains(GraphNode::resolution(attr("OX"))));     // no edge to itself

    // Inspecting computed nothing and detected nothing.
    QCOMPARE(w.engine.totalRunCount(), 1);
    const int cycles = w.engine.cycleCount();
    QCOMPARE(cycles, 2);                            // oR and oS each closed a ring on OX

    // Served from the cache at the top level: no evaluation at all.
    QCOMPARE(w.engine.attribute("OX"), QVariant(100));
    QCOMPARE(w.engine.cycleCount(), cycles);
    QCOMPARE(w.engine.totalRunCount(), 1);

    // The evaluation of OY may not use the cached OX: beneath OY, the first
    // candidate of OX leads back to OY. The cached oQ is used (not run again).
    QVERIFY(!w.engine.attribute("OY").isValid());
    QVERIFY(w.engine.cycleCount() > cycles);
    QCOMPARE(w.engine.totalRunCount(), 1);
    QCOMPARE(w.engine.cachedState(attr("OY")), CachedState::Unavailable);
    QCOMPARE(w.engine.resultStatus("oS"), std::optional<ResultStatus>(ResultStatus::Cycle));
    QVERIFY(w.engine.verifyAgainstFresh(Synthetic::tangleNames()).isEmpty());

    // Invalidation drops the provisional verdicts with the answers that held them.
    w.engine.clear();
    QCOMPARE(w.engine.resultStatus("oP"), std::optional<ResultStatus>());
    QCOMPARE(w.engine.cachedNodeCount(), 0);
    QCOMPARE(w.engine.edgeCount(), 0);
}

// A ring that is closed entirely beneath a node does not make that node
// provisional: the P/R ring closes at X2, so X2 and the acyclic chain above it
// (h, H, g, G) are cached, and a second read runs nothing.
void CalcEngineSafetyTest::ringBeneathAcyclicAncestorIsCached()
{
    using CachedState = CalculationEngine::CachedState;
    World w;
    QVERIFY(w.registry.registerCalculation(plus("h", "X2", "H", 1)));
    QVERIFY(w.registry.registerCalculation(plus("g", "H", "G", 1)));
    QVERIFY(w.registry.registerCalculation(plus("g2", "H", "G2", 2)));

    QCOMPARE(w.engine.attribute("G"), QVariant(102));
    QVERIFY(w.engine.cycleCount() >= 1);
    QCOMPARE(w.engine.runCount("g"), 1);
    QCOMPARE(w.engine.runCount("h"), 1);
    QCOMPARE(w.engine.runCount("Q"), 1);
    QCOMPARE(w.engine.runCount("S"), 1);
    QCOMPARE(w.engine.totalRunCount(), 4);
    QCOMPARE(w.engine.cachedState(attr("G")), CachedState::Available);
    QCOMPARE(w.engine.cachedState(attr("H")), CachedState::Available);
    QCOMPARE(w.engine.cachedState(attr("X2")), CachedState::Available);  // the top frame of the ring
    QCOMPARE(w.engine.cachedState(attr("Y2")), CachedState::NotCached);  // beneath it: provisional
    QCOMPARE(w.engine.resultStatus("g"), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(w.engine.resultStatus("h"), std::optional<ResultStatus>(ResultStatus::Ok));

    // Second read, and a nested read of H from another calculation: nothing but
    // g2 runs and no ring is met again.
    w.engine.resetRunCounts();
    const int cycles = w.engine.cycleCount();
    QCOMPARE(w.engine.attribute("G"), QVariant(102));
    QCOMPARE(w.engine.attribute("H"), QVariant(101));
    QCOMPARE(w.engine.attribute("X2"), QVariant(100));
    QCOMPARE(w.engine.totalRunCount(), 0);
    QCOMPARE(w.engine.attribute("G2"), QVariant(103));
    QCOMPARE(w.engine.runCount("g2"), 1);
    QCOMPARE(w.engine.totalRunCount(), 1);
    QCOMPARE(w.engine.cycleCount(), cycles);

    QList<DependencyKey> names = sharedNames();
    names << attr("G") << attr("H") << attr("G2");
    QVERIFY(w.engine.verifyAgainstFresh(names).isEmpty());

    // The chain still depends on the ring: cutting it reaches G.
    const Names dropped = w.state.setAttribute(w.engine, "Y2", 5);
    QVERIFY(dropped.contains(attr("G")));
    QVERIFY(dropped.contains(attr("G2")));
    QCOMPARE(w.engine.attribute("G"), QVariant(8));
    QVERIFY(w.engine.verifyAgainstFresh(names).isEmpty());
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
    // Only the "not requested" answers can have been dropped: whatever looked
    // at the calculation during the evaluation was provisional, never cached.
    QCOMPARE(outcome.invalidated, readFirst ? Names({attr("EX"), attr("EY")}) : Names());
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

void CalcEngineSafetyTest::requestOnRingWithFallback_data()
{
    QTest::addColumn<bool>("exFirst");
    QTest::newRow("EX then EY") << true;
    QTest::newRow("EY then EX") << false;
}

// request() obeys the caching rule too. EY has a fallback, so after the request
// EY = 7 whichever name is read first, although the cached Cycle result of the
// requested calculation may not be used beneath EY (it looked at EY).
void CalcEngineSafetyTest::requestOnRingWithFallback()
{
    QFETCH(bool, exFirst);
    World w(false);
    QVERIFY(w.registry.registerCalculation(explicitE()));
    QVERIFY(w.registry.registerCalculation(feedsE()));
    QVERIFY(w.registry.registerCalculation(constEY()));
    const QList<DependencyKey> names = {attr("EX"), attr("EY")};

    // Not requested: no ring. feedsE is missing EX, so EY falls back.
    QCOMPARE(w.engine.attribute("EY"), QVariant(7));
    QVERIFY(!w.engine.attribute("EX").isValid());
    QCOMPARE(w.engine.cycleCount(), 0);

    const CalculationEngine::RequestOutcome outcome = w.engine.request("explicitE");
    QCOMPARE(outcome.status, ResultStatus::Cycle);
    QCOMPARE(outcome.invalidated, Names({attr("EX"), attr("EY")}));
    QCOMPARE(w.engine.resultStatus("explicitE"), std::optional<ResultStatus>(ResultStatus::Cycle));
    QCOMPARE(w.engine.cachedState(attr("EX")), CalculationEngine::CachedState::NotCached);
    QCOMPARE(w.engine.cachedState(attr("EY")), CalculationEngine::CachedState::NotCached);

    if (exFirst) {
        QVERIFY(!w.engine.attribute("EX").isValid());
        QCOMPARE(w.engine.attribute("EY"), QVariant(7));
    } else {
        QCOMPARE(w.engine.attribute("EY"), QVariant(7));
        QVERIFY(!w.engine.attribute("EX").isValid());
    }
    QCOMPARE(w.engine.runCount("explicitE"), 0);
    QCOMPARE(w.engine.runCount("feedsE"), 0);
    QVERIFY(w.engine.verifyAgainstFresh(names).isEmpty());

    // Still requested, still a valid result.
    QCOMPARE(w.engine.request("explicitE").status, ResultStatus::Cycle);
    QVERIFY(w.engine.request("explicitE").invalidated.isEmpty());
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
