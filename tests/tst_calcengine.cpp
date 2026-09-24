// CalculationEngine: resolution, caching, dependency recording, invalidation,
// cross-session broadcast, explicit policy, preferences, families, inspection,
// and the measurement layers - all with synthetic calculations against a fake
// session state. Expected values are literals.
//
// Acceptance items (tests/README.md, appendix A) are named on the test functions that
// demonstrate them at engine level.

#include <optional>

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

// Declaration order matters: a registry must outlive its engines.
struct World {
    CalculationRegistry registry;
    FakePreferenceProvider prefs;
    FakeSessionState state;
    CalculationEngine engine;
    QList<Names> broadcasts;    // what the invalidation listener received

    explicit World(bool sharedWorld = true)
        : engine(&state, &registry)
    {
        registry.setPreferenceProvider(&prefs);
        if (sharedWorld)
            Synthetic::registerSharedWorld(registry);
        engine.setInvalidationListener([this](const Names &names) { broadcasts.append(names); });
    }

    void setAbp()   // the usual starting point: A=1, B=2, p=5
    {
        state.setAttribute("A", 1);
        state.setAttribute("B", 2);
        prefs.set("p", 5);
    }
};

const QVector<double> kOneTwoThree{1.0, 2.0, 3.0};

} // namespace

class CalcEngineTest : public QObject {
    Q_OBJECT

private slots:
    void storedWins();
    void threeOutputsRunOnce();
    void declaredInputChangeRunsOnceMore();
    void unrelatedChangeRunsNothing();
    void preferredCandidateReplacesFallback();
    void rejectedCandidatesAreDependencies();
    void removingStoredFallsBackToCalc();
    void storedInvalidValueStillWins();
    void partialResultNextCandidate();
    void unavailableIsCached();
    void overrideOneOutput();
    void overrideFeedsDownstream();
    void twoSessionsIndependent();
    void unregisterInvalidatesEverySession();
    void registerInvalidatesNegativeCache();
    void explicitPolicy();
    void declaredPreferenceInvalidates();
    void snapshottedPreferenceDoesNot();
    void missingPreferenceIsUnavailable();
    void familyInstancesAreDistinct();
    void unregisterFamilyInvalidatesInstances();
    void inspectDoesNotCompute();
    void invalidateDoesNotCompute();
    void transitiveInvalidation();
    void edgesAreCleanedUp();
    void measurementPassthrough();
    void derivedMeasurementAndUnit();
    void sourceConversionHook();
    void undeclaredReadDetected();
    void invalidOutputRejected();
    void clearDropsEverything();
    void rebindKeepsCaches();
    // Needed by the Python plugin view
    void isDeclaredIsSilent();
};

void CalcEngineTest::storedWins()
{
    World w;
    w.setAbp();
    w.state.setAttribute("X", 7);

    QCOMPARE(w.engine.attribute("X"), QVariant(7));
    QVERIFY(w.engine.isAvailable(attr("X")));
    // No candidate was consulted, and the name depends on the stored leaf only.
    QCOMPARE(w.engine.runCount("sum"), 0);
    QCOMPARE(w.engine.totalRunCount(), 0);
    QCOMPARE(w.engine.resultStatus("sum"), std::optional<ResultStatus>());
    QCOMPARE(w.engine.dependenciesOf(GraphNode::resolution(attr("X"))),
             QSet<GraphNode>({GraphNode::storedAttribute("X")}));

    // Same for a stored measurement.
    w.state.setMeasurement("S", "d", {4.0, 5.0}, "stored");
    QCOMPARE(w.engine.measurement("S", "d"), (QVector<double>{4.0, 5.0}));
    QCOMPARE(w.engine.measurementUnit("S", "d"), QStringLiteral("stored"));
    QCOMPARE(w.engine.runCount("meas"), 0);
    QVERIFY(w.engine.dependenciesOf(GraphNode::resolution(measKey("S", "d")))
                .contains(GraphNode::sourceMeasurement("S", "d")));
    QCOMPARE(w.engine.totalRunCount(), 0);
}

// Acceptance 9: a multi-output calculation runs once whichever output is read
// first and however many are read.
void CalcEngineTest::threeOutputsRunOnce()
{
    World w;
    w.setAbp();

    QCOMPARE(w.engine.attribute("Z"), QVariant(6));
    QCOMPARE(w.engine.attribute("Y"), QVariant(8));
    QCOMPARE(w.engine.attribute("W"), QVariant(9));
    QCOMPARE(w.engine.attribute("Y"), QVariant(8));
    QCOMPARE(w.engine.attribute("Z"), QVariant(6));
    QCOMPARE(w.engine.runCount("triple"), 1);
    QCOMPARE(w.engine.runCount("sum"), 1);
    QCOMPARE(w.engine.runCount("wAlt"), 0);

    // A second engine over the same state, reading in another order.
    CalculationEngine other(&w.state, &w.registry);
    QCOMPARE(other.attribute("W"), QVariant(9));
    QCOMPARE(other.attribute("Z"), QVariant(6));
    QCOMPARE(other.attribute("Y"), QVariant(8));
    for (int i = 0; i < 10; ++i)
        QCOMPARE(other.attribute("W"), QVariant(9));
    QCOMPARE(other.runCount("triple"), 1);
    QCOMPARE(w.engine.runCount("triple"), 1);   // counters are per engine
}

// Acceptance 9: a change to a declared input runs it exactly once more.
void CalcEngineTest::declaredInputChangeRunsOnceMore()
{
    World w;
    w.setAbp();
    QCOMPARE(w.engine.attribute("Y"), QVariant(8));
    QCOMPARE(w.engine.runCount("triple"), 1);

    w.prefs.set(w.registry, "p", 6);
    for (int i = 0; i < 2; ++i) {
        QCOMPARE(w.engine.attribute("Y"), QVariant(9));
        QCOMPARE(w.engine.attribute("Z"), QVariant(6));
        QCOMPARE(w.engine.attribute("W"), QVariant(9));
    }
    QCOMPARE(w.engine.runCount("triple"), 2);
    QCOMPARE(w.engine.runCount("sum"), 1);      // X did not depend on p
}

// Acceptance 9: an unrelated change runs nothing.
void CalcEngineTest::unrelatedChangeRunsNothing()
{
    World w;
    w.setAbp();
    QCOMPARE(w.engine.attribute("Y"), QVariant(8));
    QCOMPARE(w.engine.attribute("W"), QVariant(9));
    const int runs = w.engine.totalRunCount();
    QCOMPARE(runs, 2);

    QCOMPARE(w.state.setAttribute(w.engine, "U", 1), Names({attr("U")}));
    QCOMPARE(w.state.setMeasurement(w.engine, "T", "q", kOneTwoThree), Names({measKey("T", "q")}));
    w.prefs.set(w.registry, "other", 1);
    QVERIFY(w.broadcasts.isEmpty());

    QCOMPARE(w.engine.cachedState(attr("Y")), CalculationEngine::CachedState::Available);
    QCOMPARE(w.engine.attribute("Y"), QVariant(8));
    QCOMPARE(w.engine.attribute("Z"), QVariant(6));
    QCOMPARE(w.engine.attribute("W"), QVariant(9));
    QCOMPARE(w.engine.attribute("X"), QVariant(3));
    QCOMPARE(w.engine.totalRunCount(), runs);
}

// Acceptance 10: a cached fallback is replaced when the preferred candidate's
// inputs arrive, whether or not the value was read beforehand.
void CalcEngineTest::preferredCandidateReplacesFallback()
{
    World w;
    w.state.setAttribute("C", 4);
    QCOMPARE(w.engine.attribute("X"), QVariant(40));
    QCOMPARE(w.engine.runCount("fallbackX"), 1);

    // First missing input of the preferred candidate arrives: X is invalidated,
    // re-resolves, and still falls back - without re-running the fallback.
    const Names afterA = w.state.setAttribute(w.engine, "A", 1);
    QVERIFY(afterA.contains(attr("A")));
    QVERIFY(afterA.contains(attr("X")));
    QCOMPARE(w.engine.cachedState(attr("X")), CalculationEngine::CachedState::NotCached);
    QCOMPARE(w.engine.attribute("X"), QVariant(40));
    QCOMPARE(w.engine.runCount("fallbackX"), 1);
    QCOMPARE(w.engine.runCount("sum"), 0);

    const Names afterB = w.state.setAttribute(w.engine, "B", 2);
    QVERIFY(afterB.contains(attr("X")));
    QCOMPARE(w.engine.attribute("X"), QVariant(3));
    QCOMPARE(w.engine.runCount("sum"), 1);
    QCOMPARE(w.engine.runCount("fallbackX"), 1);

    // Never read before the inputs arrived: same answer.
    World unread;
    unread.state.setAttribute("C", 4);
    unread.state.setAttribute(unread.engine, "A", 1);
    unread.state.setAttribute(unread.engine, "B", 2);
    QCOMPARE(unread.engine.attribute("X"), QVariant(3));
    QCOMPARE(unread.engine.runCount("fallbackX"), 0);
}

void CalcEngineTest::rejectedCandidatesAreDependencies()
{
    World w;
    w.state.setAttribute("C", 4);
    QCOMPARE(w.engine.attribute("X"), QVariant(40));

    // The stored value's absence, the rejected candidate, and the winner. Not
    // constX: resolution stopped at the winner.
    QCOMPARE(w.engine.dependenciesOf(GraphNode::resolution(attr("X"))),
             QSet<GraphNode>({GraphNode::storedAttribute("X"), GraphNode::result("sum"),
                              GraphNode::result("fallbackX")}));
    QCOMPARE(w.engine.resultStatus("sum"), std::optional<ResultStatus>(ResultStatus::MissingInput));
    QCOMPARE(w.engine.resultStatus("fallbackX"), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(w.engine.resultStatus("constX"), std::optional<ResultStatus>());

    // The availability pass stopped at the first missing input (A); B was not looked at.
    QCOMPARE(w.engine.dependenciesOf(GraphNode::result("sum")),
             QSet<GraphNode>({GraphNode::resolution(attr("A"))}));
    QCOMPARE(w.engine.dependenciesOf(GraphNode::resolution(attr("A"))),
             QSet<GraphNode>({GraphNode::storedAttribute("A")}));
    QCOMPARE(w.engine.cachedState(attr("A")), CalculationEngine::CachedState::Unavailable);
    QCOMPARE(w.engine.cachedState(attr("B")), CalculationEngine::CachedState::NotCached);
}

void CalcEngineTest::removingStoredFallsBackToCalc()
{
    World w;
    w.setAbp();
    w.state.setAttribute("X", 7);
    QCOMPARE(w.engine.attribute("X"), QVariant(7));

    const Names names = w.state.removeAttribute(w.engine, "X");
    QVERIFY(names.contains(attr("X")));
    QCOMPARE(w.engine.attribute("X"), QVariant(3));
    QCOMPARE(w.engine.runCount("sum"), 1);

    // And back: storing it again overrides the calculation.
    QVERIFY(w.state.setAttribute(w.engine, "X", 8).contains(attr("X")));
    QCOMPARE(w.engine.attribute("X"), QVariant(8));
    QCOMPARE(w.engine.runCount("sum"), 1);
}

void CalcEngineTest::storedInvalidValueStillWins()
{
    // A stored attribute wins resolution even if its stored QVariant is invalid;
    // the name is then unavailable and no candidate is tried.
    World w;
    w.setAbp();
    w.state.setAttribute("X", QVariant());
    QVERIFY(!w.engine.attribute("X").isValid());
    QVERIFY(!w.engine.isAvailable(attr("X")));
    QCOMPARE(w.engine.totalRunCount(), 0);

    // Likewise a present but empty source measurement.
    w.state.setAttribute("Y", 1);
    w.state.setMeasurement("S", "d", {});
    QVERIFY(w.engine.measurement("S", "d").isEmpty());
    QCOMPARE(w.engine.runCount("meas"), 0);
}

void CalcEngineTest::partialResultNextCandidate()
{
    World w;
    QVERIFY(w.registry.unregister("sum", CalculationRegistry::Removal::Change));
    QVERIFY(w.registry.unregister("fallbackX", CalculationRegistry::Removal::Change));
    w.prefs.set("p", 0);

    // X = -1, so triple provides Y and Z but reports W unavailable; W falls to
    // the next candidate while Y and Z stay readable from the same run.
    QCOMPARE(w.engine.attribute("W"), QVariant(998));
    QCOMPARE(w.engine.attribute("Y"), QVariant(-1));
    QCOMPARE(w.engine.attribute("Z"), QVariant(-2));
    QCOMPARE(w.engine.runCount("triple"), 1);
    QCOMPARE(w.engine.runCount("wAlt"), 1);
    QCOMPARE(w.engine.resultStatus("triple"), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(w.engine.dependenciesOf(GraphNode::resolution(attr("W"))),
             QSet<GraphNode>({GraphNode::storedAttribute("W"), GraphNode::result("triple"),
                              GraphNode::result("wAlt")}));
}

void CalcEngineTest::unavailableIsCached()
{
    World w(false);
    QVERIFY(w.registry.registerCalculation(Synthetic::sum()));

    // A calculation that runs fine and provides nothing.
    CalculationDescriptor nothing;
    nothing.id = QStringLiteral("nothing");
    nothing.outputs = {attr("E")};
    nothing.compute = [](const EvaluationContext &) { return CalculationResult::unavailable(); };
    QVERIFY(w.registry.registerCalculation(nothing));

    QVERIFY(!w.engine.attribute("X").isValid());
    QVERIFY(!w.engine.attribute("E").isValid());
    const int reads = w.state.readCount();
    QVERIFY(reads > 0);

    for (int i = 0; i < 3; ++i) {
        QVERIFY(!w.engine.attribute("X").isValid());
        QVERIFY(!w.engine.isAvailable(attr("X")));
        QVERIFY(!w.engine.attribute("E").isValid());
    }
    QCOMPARE(w.state.readCount(), reads);   // served from the negative cache
    QCOMPARE(w.engine.cachedState(attr("X")), CalculationEngine::CachedState::Unavailable);
    QCOMPARE(w.engine.cachedState(attr("E")), CalculationEngine::CachedState::Unavailable);
    QCOMPARE(w.engine.resultStatus("sum"), std::optional<ResultStatus>(ResultStatus::MissingInput));
    QCOMPARE(w.engine.resultStatus("nothing"), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(w.engine.runCount("sum"), 0);
    QCOMPARE(w.engine.runCount("nothing"), 1);
}

// Acceptance 11: a user-set attribute overrides one output of a multi-output
// calculation; the others stay available and no cycle arises.
void CalcEngineTest::overrideOneOutput()
{
    World w;
    w.setAbp();
    w.state.setAttribute("Z", 50);

    QCOMPARE(w.engine.attribute("Z"), QVariant(50));
    QCOMPARE(w.engine.attribute("Y"), QVariant(8));
    QCOMPARE(w.engine.attribute("W"), QVariant(9));
    QCOMPARE(w.engine.cycleCount(), 0);
    QCOMPARE(w.engine.runCount("triple"), 1);
    QCOMPARE(w.engine.runCount("wAlt"), 0);
    QCOMPARE(w.engine.dependenciesOf(GraphNode::resolution(attr("Z"))),
             QSet<GraphNode>({GraphNode::storedAttribute("Z")}));

    // Removing the override exposes the value triple computed all along.
    const Names names = w.state.removeAttribute(w.engine, "Z");
    QCOMPARE(names, Names({attr("Z")}));
    QCOMPARE(w.engine.attribute("Z"), QVariant(6));
    QCOMPARE(w.engine.runCount("triple"), 1);
    QCOMPARE(w.engine.runCount("wAlt"), 0);
    QVERIFY(w.engine.verifyAgainstFresh({attr("X"), attr("Y"), attr("Z"), attr("W")}).isEmpty());
}

// Acceptance 11: the override, not the calculated value, feeds downstream.
void CalcEngineTest::overrideFeedsDownstream()
{
    World w;
    QVERIFY(w.registry.unregister("sum", CalculationRegistry::Removal::Change));
    QVERIFY(w.registry.unregister("fallbackX", CalculationRegistry::Removal::Change));
    w.prefs.set("p", 0);
    w.state.setAttribute("Z", 50);

    QCOMPARE(w.engine.attribute("W"), QVariant(1050));
    QCOMPARE(w.engine.attribute("Z"), QVariant(50));
    QCOMPARE(w.engine.attribute("Y"), QVariant(-1));
    QCOMPARE(w.engine.cycleCount(), 0);
    QCOMPARE(w.engine.runCount("triple"), 1);
}

// Acceptance 13: sessions are independent.
void CalcEngineTest::twoSessionsIndependent()
{
    CalculationRegistry registry;
    Synthetic::registerSharedWorld(registry);
    FakeSessionState state1, state2;
    state1.setAttribute("A", 1);
    state1.setAttribute("B", 2);
    state2.setAttribute("A", 10);
    state2.setAttribute("B", 20);
    CalculationEngine engine1(&state1, &registry);
    CalculationEngine engine2(&state2, &registry);

    QCOMPARE(engine1.attribute("X"), QVariant(3));
    QCOMPARE(engine2.attribute("X"), QVariant(30));

    QVERIFY(state1.setAttribute(engine1, "A", 5).contains(attr("X")));
    QCOMPARE(engine2.cachedState(attr("X")), CalculationEngine::CachedState::Available);
    QCOMPARE(engine1.attribute("X"), QVariant(7));
    QCOMPARE(engine2.attribute("X"), QVariant(30));
    QCOMPARE(engine1.runCount("sum"), 2);
    QCOMPARE(engine2.runCount("sum"), 1);
    QCOMPARE(engine2.totalRunCount(), 1);
}

// Acceptance 13: a registry change invalidates affected results in every
// loaded session; no cache entry outlives its registration.
void CalcEngineTest::unregisterInvalidatesEverySession()
{
    CalculationRegistry registry;
    QVERIFY(registry.registerCalculation(Synthetic::sum()));
    FakeSessionState state1, state2;
    state1.setAttribute("A", 1);
    state1.setAttribute("B", 2);
    state2.setAttribute("A", 10);
    state2.setAttribute("B", 20);
    CalculationEngine engine1(&state1, &registry);
    CalculationEngine engine2(&state2, &registry);
    QList<Names> received1, received2;
    engine1.setInvalidationListener([&](const Names &n) { received1.append(n); });
    engine2.setInvalidationListener([&](const Names &n) { received2.append(n); });

    QCOMPARE(engine1.attribute("X"), QVariant(3));
    QCOMPARE(engine2.attribute("X"), QVariant(30));
    state1.resetReadCount();
    state2.resetReadCount();

    QVERIFY(registry.unregister("sum", CalculationRegistry::Removal::Change));

    QCOMPARE(received1.size(), 1);
    QCOMPARE(received2.size(), 1);
    QVERIFY(received1.first().contains(attr("X")));
    QVERIFY(received2.first().contains(attr("X")));
    QCOMPARE(engine1.resultStatus("sum"), std::optional<ResultStatus>());
    QCOMPARE(engine2.resultStatus("sum"), std::optional<ResultStatus>());
    QCOMPARE(engine1.cachedState(attr("X")), CalculationEngine::CachedState::NotCached);
    QCOMPARE(engine2.cachedState(attr("X")), CalculationEngine::CachedState::NotCached);
    // The unregister itself computed nothing and read no state.
    QCOMPARE(engine1.totalRunCount(), 1);
    QCOMPARE(engine2.totalRunCount(), 1);
    QCOMPARE(state1.readCount(), 0);
    QCOMPARE(state2.readCount(), 0);

    QVERIFY(!engine1.attribute("X").isValid());
    QVERIFY(!engine2.attribute("X").isValid());
    QCOMPARE(engine1.totalRunCount(), 1);
}

// Acceptance 13: a newly registered calculation reaches names cached as "none".
void CalcEngineTest::registerInvalidatesNegativeCache()
{
    CalculationRegistry registry;
    FakeSessionState state1, state2;
    CalculationEngine engine1(&state1, &registry);
    CalculationEngine engine2(&state2, &registry);
    QList<Names> received1, received2;
    engine1.setInvalidationListener([&](const Names &n) { received1.append(n); });
    engine2.setInvalidationListener([&](const Names &n) { received2.append(n); });

    QVERIFY(!engine1.attribute("X").isValid());
    QVERIFY(!engine2.attribute("X").isValid());
    QCOMPARE(engine1.cachedState(attr("X")), CalculationEngine::CachedState::Unavailable);

    QVERIFY(registry.registerCalculation(Synthetic::constX()));
    QCOMPARE(received1, QList<Names>({Names({attr("X")})}));
    QCOMPARE(received2, QList<Names>({Names({attr("X")})}));
    QCOMPARE(engine1.totalRunCount(), 0);   // registering computed nothing
    QCOMPARE(engine1.attribute("X"), QVariant(-1));
    QCOMPARE(engine2.attribute("X"), QVariant(-1));

    // A preferred candidate cannot be inserted ahead (order is registration
    // order), but a registration for a name nobody has read notifies nobody.
    received1.clear();
    QVERIFY(registry.registerCalculation(Synthetic::wAlt()));
    QVERIFY(received1.isEmpty());
}

// Acceptance 14: an explicit calculation reports unavailable until requested,
// then publishes one result for all outputs.
void CalcEngineTest::explicitPolicy()
{
    World w(false);
    CalculationDescriptor constW;
    constW.id = QStringLiteral("constW");
    constW.outputs = {attr("W")};
    constW.compute = [](const EvaluationContext &) { return CalculationResult().setAttribute("W", 77); };
    QVERIFY(w.registry.registerCalculation(Synthetic::sum()));
    QVERIFY(w.registry.registerCalculation(Synthetic::triple(EvaluationPolicy::Explicit)));
    QVERIFY(w.registry.registerCalculation(constW));
    w.setAbp();

    // Before the request: unavailable, no run; a later on-demand candidate wins.
    QVERIFY(!w.engine.attribute("Y").isValid());
    QVERIFY(!w.engine.attribute("Z").isValid());
    QCOMPARE(w.engine.attribute("W"), QVariant(77));
    QCOMPARE(w.engine.runCount("triple"), 0);
    QCOMPARE(w.engine.resultStatus("triple"), std::optional<ResultStatus>(ResultStatus::NotRequested));
    QCOMPARE(w.engine.runCount("sum"), 0);      // not even its inputs were looked at

    const CalculationEngine::RequestOutcome first = w.engine.request("triple");
    QVERIFY(first.found);
    QCOMPARE(first.status, ResultStatus::Ok);
    QCOMPARE(first.invalidated, Names({attr("Y"), attr("Z"), attr("W")}));
    QVERIFY(w.broadcasts.isEmpty());            // returned to the caller, not broadcast
    QCOMPARE(w.engine.attribute("Y"), QVariant(8));
    QCOMPARE(w.engine.attribute("Z"), QVariant(6));
    QCOMPARE(w.engine.attribute("W"), QVariant(9));
    QCOMPARE(w.engine.runCount("triple"), 1);

    const CalculationEngine::RequestOutcome second = w.engine.request("triple");
    QVERIFY(second.found);
    QCOMPARE(second.status, ResultStatus::Ok);
    QVERIFY(second.invalidated.isEmpty());
    QCOMPARE(w.engine.runCount("triple"), 1);
    QVERIFY(w.engine.verifyAgainstFresh({attr("X"), attr("Y"), attr("Z"), attr("W")}).isEmpty());

    // A declared input changes: back to "not requested"; reads never start it.
    const Names changed = w.state.setAttribute(w.engine, "A", 2);
    QVERIFY(changed.contains(attr("Y")));
    QVERIFY(changed.contains(attr("Z")));
    QVERIFY(changed.contains(attr("W")));
    QVERIFY(!w.engine.attribute("Y").isValid());
    QVERIFY(!w.engine.attribute("Z").isValid());
    QCOMPARE(w.engine.attribute("W"), QVariant(77));
    QCOMPARE(w.engine.runCount("triple"), 1);
    QCOMPARE(w.engine.resultStatus("triple"), std::optional<ResultStatus>(ResultStatus::NotRequested));
    QVERIFY(w.engine.verifyAgainstFresh({attr("X"), attr("Y"), attr("Z"), attr("W")}).isEmpty());

    QCOMPARE(w.engine.request("triple").status, ResultStatus::Ok);
    QCOMPARE(w.engine.attribute("Y"), QVariant(9));
    QCOMPARE(w.engine.attribute("Z"), QVariant(8));
    QCOMPARE(w.engine.attribute("W"), QVariant(12));
    QCOMPARE(w.engine.runCount("triple"), 2);

    // Unknown identity: nothing happens. On-demand identity: an eager evaluation.
    QVERIFY(!w.engine.request("nope").found);
    QVERIFY(w.engine.request("sum", attr("ignoredForAPlainCalculation")).found);
    QVERIFY(!w.engine.request("sum#x").found);
    QCOMPARE(w.engine.request("constW").status, ResultStatus::Ok);
    QCOMPARE(w.engine.runCount("constW"), 1);
    QCOMPARE(w.engine.scopeDepth(), 0);
}

// Acceptance 15: changing a preference a calculation declares invalidates its results.
void CalcEngineTest::declaredPreferenceInvalidates()
{
    World w;
    w.setAbp();
    QCOMPARE(w.engine.attribute("Y"), QVariant(8));
    QCOMPARE(w.engine.attribute("Z"), QVariant(6));
    QCOMPARE(w.engine.attribute("W"), QVariant(9));
    QCOMPARE(w.engine.attribute("X"), QVariant(3));

    w.prefs.set(w.registry, "p", 6);
    QCOMPARE(w.broadcasts, QList<Names>({Names({attr("Y"), attr("Z"), attr("W")})}));
    QCOMPARE(w.engine.cachedState(attr("X")), CalculationEngine::CachedState::Available);

    QCOMPARE(w.engine.attribute("Y"), QVariant(9));
    QCOMPARE(w.engine.attribute("Z"), QVariant(6));
    QCOMPARE(w.engine.attribute("W"), QVariant(9));
    QCOMPARE(w.engine.dependenciesOf(GraphNode::result("triple")),
             QSet<GraphNode>({GraphNode::resolution(attr("X")), GraphNode::preference("p")}));
}

// Acceptance 15: a preference that was snapshotted into a session attribute at
// import is read as an attribute; changing the preference touches no session.
void CalcEngineTest::snapshottedPreferenceDoesNot()
{
    World w(false);
    CalculationDescriptor massUser;
    massUser.id = QStringLiteral("massUser");
    massUser.inputs = {CalcInput::attribute("_JUMPER_MASS")};
    massUser.outputs = {attr("M2")};
    massUser.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute("M2", ctx.attribute("_JUMPER_MASS").toInt() * 2);
    };
    QVERIFY(w.registry.registerCalculation(massUser));

    w.prefs.set("aero/mass", 80);
    w.state.setAttribute("_JUMPER_MASS", 80);   // copied "at import"
    QCOMPARE(w.engine.attribute("M2"), QVariant(160));

    w.prefs.set(w.registry, "aero/mass", 90);
    QVERIFY(w.broadcasts.isEmpty());
    QCOMPARE(w.engine.cachedState(attr("M2")), CalculationEngine::CachedState::Available);
    QCOMPARE(w.engine.attribute("M2"), QVariant(160));
    QCOMPARE(w.engine.runCount("massUser"), 1);
    QCOMPARE(w.prefs.readCount(), 0);           // the engine never asked for it
}

void CalcEngineTest::missingPreferenceIsUnavailable()
{
    World w;
    w.state.setAttribute("A", 1);
    w.state.setAttribute("B", 2);               // p is not set

    QVERIFY(!w.engine.attribute("Y").isValid());
    QCOMPARE(w.engine.resultStatus("triple"), std::optional<ResultStatus>(ResultStatus::MissingInput));
    QCOMPARE(w.engine.runCount("triple"), 0);

    // The preference appearing is a change like any other.
    w.prefs.set(w.registry, "p", 5);
    QCOMPARE(w.broadcasts.size(), 1);
    QVERIFY(w.broadcasts.first().contains(attr("Y")));
    QCOMPARE(w.engine.attribute("Y"), QVariant(8));

    // No provider at all: unavailable, not a crash.
    w.registry.setPreferenceProvider(nullptr);
    w.registry.notifyPreferenceChanged("p");
    QVERIFY(!w.engine.attribute("Y").isValid());
    QCOMPARE(w.engine.resultStatus("triple"), std::optional<ResultStatus>(ResultStatus::MissingInput));
}

// One registration, one instance per parameter set, each with its own
// result and dependencies.
void CalcEngineTest::familyInstancesAreDistinct()
{
    World w;
    w.setAbp();

    QCOMPARE(w.engine.attribute("neg:A"), QVariant(-1));
    QCOMPARE(w.engine.attribute("neg:B"), QVariant(-2));
    QCOMPARE(w.engine.runCount("neg"), 2);
    QCOMPARE(w.engine.runCountForInstance("neg#neg:A"), 1);
    QCOMPARE(w.engine.runCountForInstance("neg#neg:B"), 1);
    QCOMPARE(w.engine.resultStatus("neg", attr("neg:A")), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(w.engine.resultStatus("neg", attr("neg:C")), std::optional<ResultStatus>());
    QCOMPARE(w.engine.dependenciesOf(GraphNode::result("neg#neg:A")),
             QSet<GraphNode>({GraphNode::resolution(attr("A"))}));

    const Names names = w.engine.attributeChanged("A");
    QVERIFY(names.contains(attr("neg:A")));
    QVERIFY(!names.contains(attr("neg:B")));

    QCOMPARE(w.engine.attribute("neg:B"), QVariant(-2));
    QCOMPARE(w.engine.runCount("neg"), 2);      // re-reading neg:B ran nothing
    QCOMPARE(w.engine.attribute("neg:A"), QVariant(-1));
    QCOMPARE(w.engine.runCountForInstance("neg#neg:A"), 2);
    QCOMPARE(w.engine.runCountForInstance("neg#neg:B"), 1);

    // An instance over a calculated input nests like any other calculation.
    QCOMPARE(w.engine.attribute("neg:X"), QVariant(-3));
    QCOMPARE(w.engine.request("neg", attr("neg:X")).status, ResultStatus::Ok);
    QCOMPARE(w.engine.runCountForInstance("neg#neg:X"), 1);
}

void CalcEngineTest::unregisterFamilyInvalidatesInstances()
{
    World w;
    w.setAbp();
    QCOMPARE(w.engine.attribute("neg:A"), QVariant(-1));
    QVERIFY(!w.engine.attribute("neg:missing").isValid());
    QCOMPARE(w.engine.attribute("X"), QVariant(3));
    const int runs = w.engine.totalRunCount();

    QVERIFY(w.registry.unregister("neg", CalculationRegistry::Removal::Change));
    QCOMPARE(w.broadcasts, QList<Names>({Names({attr("neg:A"), attr("neg:missing")})}));
    QCOMPARE(w.engine.resultStatus("neg#neg:A"), std::optional<ResultStatus>());
    QCOMPARE(w.engine.cachedState(attr("X")), CalculationEngine::CachedState::Available);
    QVERIFY(!w.engine.attribute("neg:A").isValid());
    QCOMPARE(w.engine.totalRunCount(), runs);

    // Registering the family again reaches the names cached as "none".
    w.broadcasts.clear();
    QVERIFY(w.registry.registerFamily(Synthetic::neg()));
    QCOMPARE(w.broadcasts, QList<Names>({Names({attr("neg:A")})}));
    QCOMPARE(w.engine.attribute("neg:A"), QVariant(-1));
}

// Inspecting never triggers computation.
void CalcEngineTest::inspectDoesNotCompute()
{
    World w;
    w.setAbp();
    w.state.setMeasurement("S", "m", kOneTwoThree);

    const CalculationEngine &inspect = w.engine;
    QCOMPARE(inspect.cachedState(attr("X")), CalculationEngine::CachedState::NotCached);
    QCOMPARE(inspect.cachedState(measKey("S", "d")), CalculationEngine::CachedState::NotCached);
    QCOMPARE(inspect.cachedState(measKey("S", "m")), CalculationEngine::CachedState::NotCached);
    QCOMPARE(inspect.resultStatus("sum"), std::optional<ResultStatus>());
    QCOMPARE(inspect.resultStatus("neg", attr("neg:A")), std::optional<ResultStatus>());
    QCOMPARE(inspect.resultStatus("unknown"), std::optional<ResultStatus>());

    QCOMPARE(w.engine.totalRunCount(), 0);
    QCOMPARE(w.engine.cachedNodeCount(), 0);
    QCOMPARE(w.engine.edgeCount(), 0);
    QCOMPARE(w.state.readCount(), 0);
    QCOMPARE(w.prefs.readCount(), 0);

    // Still true once something is cached.
    QCOMPARE(w.engine.attribute("X"), QVariant(3));
    const int nodes = w.engine.cachedNodeCount();
    const int reads = w.state.readCount();
    QCOMPARE(inspect.cachedState(attr("X")), CalculationEngine::CachedState::Available);
    QCOMPARE(inspect.cachedState(attr("Y")), CalculationEngine::CachedState::NotCached);
    QCOMPARE(inspect.resultStatus("sum"), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(inspect.resultStatus("triple"), std::optional<ResultStatus>());
    QCOMPARE(w.engine.cachedNodeCount(), nodes);
    QCOMPARE(w.state.readCount(), reads);
    QCOMPARE(w.engine.totalRunCount(), 1);
}

// Invalidating never triggers computation.
void CalcEngineTest::invalidateDoesNotCompute()
{
    World w;
    w.setAbp();
    w.state.setAttribute("C", 4);
    w.state.setMeasurement("S", "m", kOneTwoThree, "raw");
    QCOMPARE(w.engine.attribute("W"), QVariant(9));
    QCOMPARE(w.engine.measurement("S", "d"), (QVector<double>{9.0, 10.0, 11.0}));
    QCOMPARE(w.engine.attribute("neg:A"), QVariant(-1));
    QCOMPARE(w.engine.attribute("X2"), QVariant(100));

    const int runs = w.engine.totalRunCount();
    w.state.resetReadCount();
    w.prefs.resetReadCount();

    w.engine.attributeChanged("A");
    w.engine.attributeChanged("neverSeen");
    w.engine.sourceMeasurementChanged("S", "m");
    w.engine.sourceUnitChanged("S", "m");
    w.registry.notifyPreferenceChanged("p");
    QVERIFY(w.registry.unregister("fallbackX", CalculationRegistry::Removal::Change));
    QVERIFY(w.registry.registerCalculation(Synthetic::fallbackX()));
    QVERIFY(w.registry.unregister("neg", CalculationRegistry::Removal::Change));
    QVERIFY(w.registry.registerFamily(Synthetic::neg()));
    w.engine.clear();

    QCOMPARE(w.engine.totalRunCount(), runs);
    QCOMPARE(w.state.readCount(), 0);
    QCOMPARE(w.prefs.readCount(), 0);
    QCOMPARE(w.engine.scopeDepth(), 0);
}

void CalcEngineTest::transitiveInvalidation()
{
    // A chain of three calculations: sum -> triple -> meas.
    World w;
    w.setAbp();
    w.state.setMeasurement("S", "m", kOneTwoThree);
    QCOMPARE(w.engine.measurement("S", "d"), (QVector<double>{9.0, 10.0, 11.0}));
    QCOMPARE(w.engine.totalRunCount(), 3);

    // The changed name plus every resolved name downstream; Z and W were never read.
    QCOMPARE(w.state.setAttribute(w.engine, "A", 2),
             Names({attr("A"), attr("X"), attr("Y"), measKey("S", "d")}));
    QCOMPARE(w.engine.cachedState(measKey("S", "d")), CalculationEngine::CachedState::NotCached);
    QCOMPARE(w.engine.cachedState(measKey("S", "m")), CalculationEngine::CachedState::Available);
    QCOMPARE(w.engine.cachedState(attr("B")), CalculationEngine::CachedState::Available);
    QCOMPARE(w.engine.totalRunCount(), 3);

    QCOMPARE(w.engine.measurement("S", "d"), (QVector<double>{10.0, 11.0, 12.0}));
    QCOMPARE(w.engine.totalRunCount(), 6);
}

void CalcEngineTest::edgesAreCleanedUp()
{
    World w;
    w.setAbp();
    w.state.setMeasurement("S", "m", kOneTwoThree);

    const auto readAll = [&w]() {
        w.engine.attribute("X");
        w.engine.attribute("Y");
        w.engine.attribute("Z");
        w.engine.attribute("W");
        w.engine.attribute("neg:A");
        w.engine.measurement("S", "d");
    };
    readAll();
    const int edges = w.engine.edgeCount();
    const int nodes = w.engine.cachedNodeCount();
    QVERIFY(edges > 0);

    // Invalidated nodes keep no edges at all.
    w.state.setAttribute(w.engine, "A", 3);
    const QList<GraphNode> invalidated = {
        GraphNode::resolution(attr("A")),   GraphNode::result("sum"),
        GraphNode::resolution(attr("X")),   GraphNode::result("triple"),
        GraphNode::resolution(attr("Y")),   GraphNode::resolution(attr("Z")),
        GraphNode::resolution(attr("W")),   GraphNode::result("meas"),
        GraphNode::resolution(measKey("S", "d")), GraphNode::result("neg#neg:A"),
        GraphNode::resolution(attr("neg:A")),
    };
    int remaining = 0;
    for (const GraphNode &n : invalidated)
        remaining += int(w.engine.dependenciesOf(n).size());
    QCOMPARE(remaining, 0);
    QVERIFY(w.engine.edgeCount() < edges);

    // Edit + read, over and over: the graph does not grow.
    for (int i = 0; i < 1000; ++i) {
        w.state.setAttribute(w.engine, "A", i % 5);
        readAll();
        if (w.engine.edgeCount() != edges || w.engine.cachedNodeCount() != nodes)
            QFAIL(qPrintable(QStringLiteral("graph grew at iteration %1").arg(i)));
    }
    QCOMPARE(w.engine.edgeCount(), edges);
    QCOMPARE(w.engine.cachedNodeCount(), nodes);
    QCOMPARE(w.engine.attribute("X"), QVariant(6));     // A = 999 % 5 = 4, B = 2
}

void CalcEngineTest::measurementPassthrough()
{
    World w;
    w.state.setMeasurement("S", "m", kOneTwoThree, "raw");

    QCOMPARE(w.engine.measurement("S", "m"), kOneTwoThree);
    QCOMPARE(w.engine.measurementUnit("S", "m"), QStringLiteral("raw"));
    QVERIFY(w.engine.isAvailable(measKey("S", "m")));
    QCOMPARE(w.engine.totalRunCount(), 0);
    QCOMPARE(w.engine.dependenciesOf(GraphNode::resolution(measKey("S", "m"))),
             QSet<GraphNode>({GraphNode::sourceMeasurement("S", "m"), GraphNode::sourceUnit("S", "m")}));

    // A unit change alone invalidates the effective measurement.
    QCOMPARE(w.state.setUnit(w.engine, "S", "m", "other"), Names({measKey("S", "m")}));
    QCOMPARE(w.engine.measurementUnit("S", "m"), QStringLiteral("other"));

    // Absent measurement with no candidate: unavailable, empty unit.
    QVERIFY(w.engine.measurement("S", "absent").isEmpty());
    QCOMPARE(w.engine.measurementUnit("S", "absent"), QString());
}

void CalcEngineTest::derivedMeasurementAndUnit()
{
    World w;
    w.setAbp();
    w.state.setMeasurement("S", "m", kOneTwoThree, "raw");

    QCOMPARE(w.engine.measurement("S", "d"), (QVector<double>{9.0, 10.0, 11.0}));
    QCOMPARE(w.engine.measurementUnit("S", "d"), QStringLiteral("u"));
    QCOMPARE(w.engine.runCount("meas"), 1);

    const Names names = w.state.setMeasurement(w.engine, "S", "m", {10.0, 20.0}, "raw");
    QVERIFY(names.contains(measKey("S", "m")));
    QVERIFY(names.contains(measKey("S", "d")));
    QCOMPARE(w.engine.measurement("S", "d"), (QVector<double>{18.0, 28.0}));
    QCOMPARE(w.engine.runCount("meas"), 2);
    QCOMPARE(w.engine.runCount("triple"), 1);

    // Removing the source makes the derived measurement unavailable.
    QVERIFY(w.state.removeMeasurement(w.engine, "S", "m").contains(measKey("S", "d")));
    QVERIFY(w.engine.measurement("S", "d").isEmpty());
    QCOMPARE(w.engine.resultStatus("meas"), std::optional<ResultStatus>(ResultStatus::MissingInput));
}

// The hook the conversion layer uses: for a measurement with source data
// the effective value is the conversion's output, and only a conversion may
// depend on the source layer.
void CalcEngineTest::sourceConversionHook()
{
    World w;
    w.setAbp();
    w.state.setMeasurement("S", "m", kOneTwoThree, "raw");
    w.state.setMeasurement("T", "other", kOneTwoThree, "raw");
    QCOMPARE(w.engine.measurement("S", "m"), kOneTwoThree);     // passthrough, cached

    CalculationFamily conv;
    conv.id = QStringLiteral("conv");
    conv.instantiate = [](const DependencyKey &name) -> std::optional<CalculationDescriptor> {
        if (name.type != DependencyKey::Type::Measurement || name.measurementKey.first != QLatin1String("S"))
            return std::nullopt;
        const QString sensor = name.measurementKey.first;
        const QString meas = name.measurementKey.second;
        CalculationDescriptor d;
        d.id = sensor + QLatin1Char('/') + meas;
        d.inputs = {CalcInput::sourceMeasurement(sensor, meas), CalcInput::sourceUnit(sensor, meas)};
        d.outputs = {name};
        d.compute = [sensor, meas](const EvaluationContext &ctx) {
            QVector<double> values = ctx.sourceMeasurement(sensor, meas);
            for (double &v : values)
                v *= 2.0;
            return CalculationResult().setMeasurement(sensor, meas, values,
                                                      ctx.sourceUnit(sensor, meas) == QLatin1String("raw")
                                                          ? QStringLiteral("conv") : QStringLiteral("conv?"));
        };
        return d;
    };
    QVERIFY(w.registry.registerSourceConversion(conv));
    QCOMPARE(w.broadcasts, QList<Names>({Names({measKey("S", "m")})}));

    QCOMPARE(w.engine.measurement("S", "m"), (QVector<double>{2.0, 4.0, 6.0}));
    QCOMPARE(w.engine.measurementUnit("S", "m"), QStringLiteral("conv"));
    QCOMPARE(w.engine.runCount("conv"), 1);
    QCOMPARE(w.engine.dependenciesOf(GraphNode::resolution(measKey("S", "m"))),
             QSet<GraphNode>({GraphNode::sourceMeasurement("S", "m"), GraphNode::result("conv#S/m")}));
    QCOMPARE(w.engine.dependenciesOf(GraphNode::result("conv#S/m")),
             QSet<GraphNode>({GraphNode::sourceMeasurement("S", "m"), GraphNode::sourceUnit("S", "m")}));

    // Derived values see the effective (converted) measurement.
    QCOMPARE(w.engine.measurement("S", "d"), (QVector<double>{10.0, 12.0, 14.0}));

    // With conversions registered, source data no conversion accepts is
    // unavailable; it never falls through to derived candidates.
    QVERIFY(w.engine.measurement("T", "other").isEmpty());

    const Names names = w.state.setUnit(w.engine, "S", "m", "cooked");
    QVERIFY(names.contains(measKey("S", "m")));
    QVERIFY(names.contains(measKey("S", "d")));
    QCOMPARE(w.engine.measurementUnit("S", "m"), QStringLiteral("conv?"));
    QCOMPARE(w.engine.runCount("conv"), 2);

    // Ordinary calculations cannot depend on the source layer.
    CalculationDescriptor sneaky;
    sneaky.id = QStringLiteral("sneaky");
    sneaky.inputs = {CalcInput::sourceMeasurement("S", "m")};
    sneaky.outputs = {attr("sneak")};
    sneaky.compute = [](const EvaluationContext &) { return CalculationResult(); };
    QVERIFY(!w.registry.registerCalculation(sneaky));
    QVERIFY(!w.registry.contains("sneaky"));

    // Removing the conversion restores the passthrough, including for T/other.
    w.broadcasts.clear();
    QVERIFY(w.registry.unregister("conv", CalculationRegistry::Removal::Change));
    QCOMPARE(w.broadcasts.size(), 1);
    QVERIFY(w.broadcasts.first().contains(measKey("S", "m")));
    QVERIFY(w.broadcasts.first().contains(measKey("T", "other")));
    QCOMPARE(w.engine.measurement("S", "m"), kOneTwoThree);
    QCOMPARE(w.engine.measurement("T", "other"), kOneTwoThree);
    QVERIFY(w.engine.verifyAgainstFresh({measKey("S", "m"), measKey("S", "d"), measKey("T", "other")}).isEmpty());
}

// Reading an undeclared input is an error that tests can detect.
void CalcEngineTest::undeclaredReadDetected()
{
    World w(false);
    CalculationDescriptor nosy;
    nosy.id = QStringLiteral("nosy");
    nosy.inputs = {CalcInput::attribute("A")};
    nosy.outputs = {attr("N1"), attr("N2")};
    nosy.compute = [](const EvaluationContext &ctx) {
        const int a = ctx.attribute("A").toInt();
        const QVariant other = ctx.attribute("other");      // not declared
        return CalculationResult().setAttribute("N1", a).setAttribute("N2", other.isValid() ? 1 : 0);
    };
    QVERIFY(w.registry.registerCalculation(nosy));
    w.state.setAttribute("A", 1);
    w.state.setAttribute("other", 5);

    QVERIFY(!w.engine.attribute("N1").isValid());
    QVERIFY(!w.engine.attribute("N2").isValid());
    QCOMPARE(w.engine.undeclaredReadCount(), 1);
    QCOMPARE(w.engine.lastUndeclaredRead().first, QStringLiteral("nosy"));
    QCOMPARE(w.engine.lastUndeclaredRead().second, CalcInput::attribute("other"));
    QCOMPARE(w.engine.resultStatus("nosy"), std::optional<ResultStatus>(ResultStatus::UndeclaredRead));
    QCOMPARE(w.engine.runCount("nosy"), 1);     // negatively cached: both reads, one run
    // The undeclared name was never resolved on the calculation's behalf.
    QCOMPARE(w.engine.cachedState(attr("other")), CalculationEngine::CachedState::NotCached);

    // Kind matters: a declared attribute does not license a preference read.
    CalculationDescriptor wrongKind;
    wrongKind.id = QStringLiteral("wrongKind");
    wrongKind.inputs = {CalcInput::attribute("A")};
    wrongKind.outputs = {attr("K")};
    wrongKind.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute("K", ctx.preference("A").toInt() + 1);
    };
    QVERIFY(w.registry.registerCalculation(wrongKind));
    QVERIFY(!w.engine.attribute("K").isValid());
    QCOMPARE(w.engine.undeclaredReadCount(), 2);
    QCOMPARE(w.engine.lastUndeclaredRead().second, CalcInput::preference("A"));
}

void CalcEngineTest::invalidOutputRejected()
{
    World w(false);

    CalculationDescriptor extra;
    extra.id = QStringLiteral("extra");
    extra.outputs = {attr("O1")};
    extra.compute = [](const EvaluationContext &) {
        return CalculationResult().setAttribute("O1", 1).setAttribute("notDeclared", 2);
    };
    QVERIFY(w.registry.registerCalculation(extra));

    QVERIFY(!w.engine.attribute("O1").isValid());
    QVERIFY(!w.engine.attribute("notDeclared").isValid());
    QCOMPARE(w.engine.resultStatus("extra"), std::optional<ResultStatus>(ResultStatus::InvalidOutput));

    // A declared attribute output set through setMeasurement is the wrong kind.
    CalculationDescriptor wrongKind;
    wrongKind.id = QStringLiteral("wrongKind");
    wrongKind.outputs = {attr("O2")};
    wrongKind.compute = [](const EvaluationContext &) {
        return CalculationResult().setAttribute("O2", 1).setMeasurement("O2", "", {1.0});
    };
    QVERIFY(w.registry.registerCalculation(wrongKind));
    QVERIFY(!w.engine.attribute("O2").isValid());
    QCOMPARE(w.engine.resultStatus("wrongKind"), std::optional<ResultStatus>(ResultStatus::InvalidOutput));
    QCOMPARE(w.engine.totalRunCount(), 2);
}

void CalcEngineTest::clearDropsEverything()
{
    World w;
    w.setAbp();
    QCOMPARE(w.engine.attribute("Y"), QVariant(8));
    QVERIFY(w.engine.cachedNodeCount() > 0);

    // X and Y were resolved as names; so were the inputs A and B.
    QCOMPARE(w.engine.clear(), Names({attr("A"), attr("B"), attr("X"), attr("Y")}));
    QCOMPARE(w.engine.cachedNodeCount(), 0);
    QCOMPARE(w.engine.edgeCount(), 0);
    QVERIFY(w.broadcasts.isEmpty());
    QCOMPARE(w.engine.runCount("triple"), 1);   // counters survive

    QCOMPARE(w.engine.attribute("Y"), QVariant(8));
    QCOMPARE(w.engine.runCount("triple"), 2);
}

void CalcEngineTest::rebindKeepsCaches()
{
    World w;
    w.setAbp();
    QCOMPARE(w.engine.attribute("X"), QVariant(3));

    // The owner of the state moved: same logical state at a new address.
    FakeSessionState moved = w.state;
    w.engine.rebind(&moved);
    QCOMPARE(w.engine.cachedState(attr("X")), CalculationEngine::CachedState::Available);
    QCOMPARE(w.engine.attribute("X"), QVariant(3));
    QCOMPARE(w.engine.runCount("sum"), 1);

    moved.setAttribute(w.engine, "A", 5);
    QCOMPARE(w.engine.attribute("X"), QVariant(7));
    w.engine.rebind(&w.state);      // the World destroys `moved` first
    w.engine.clear();
}

// ---- what the Python plugin view needs from the evaluation context --------

// isDeclared answers without going down the undeclared-read path.
void CalcEngineTest::isDeclaredIsSilent()
{
    World w(false);
    bool declaredA = false;
    bool declaredOther = true;
    bool declaredSourceUnit = true;

    CalculationDescriptor asks;
    asks.id = QStringLiteral("asks");
    asks.inputs = {CalcInput::attribute("A")};
    asks.outputs = {attr("ASKED")};
    asks.compute = [&](const EvaluationContext &ctx) {
        declaredA = ctx.isDeclared(CalcInput::attribute("A"));
        declaredOther = ctx.isDeclared(CalcInput::attribute("other"));
        declaredSourceUnit = ctx.isDeclared(CalcInput::sourceUnit("S", "m"));
        return CalculationResult().setAttribute("ASKED", 1);
    };
    QVERIFY(w.registry.registerCalculation(asks));
    w.state.setAttribute("A", 1);
    w.state.setAttribute("other", 5);

    QCOMPARE(w.engine.attribute("ASKED").toInt(), 1);
    QVERIFY(declaredA);
    QVERIFY(!declaredOther);
    QVERIFY(!declaredSourceUnit);
    QCOMPARE(w.engine.undeclaredReadCount(), 0);
    QCOMPARE(w.engine.resultStatus("asks"), std::optional<ResultStatus>(ResultStatus::Ok));
}

FLYSIGHT_TEST_MAIN(CalcEngineTest)
#include "tst_calcengine.moc"
