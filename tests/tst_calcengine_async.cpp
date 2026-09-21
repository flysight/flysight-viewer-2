// CalculationEngine: the asynchronous request (prepare on the main thread,
// compute anywhere, publish on the main thread), with synthetic explicit
// calculations against a fake session state. Compute is driven inline, on a
// std::thread, and on a QThread (support/asyncdriver.h).
//
// Sensor-fusion-jobs acceptance 7 (asynchronous == synchronous) and 8 (a
// declared input changing while the calculation runs refuses the result; one
// changing after publication drops it and every dependent), engine half.
//
// Expected values are literals, except where a synchronous request() on an
// identical world IS the expectation: that equality is the rule under test.

#include <atomic>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>

#include <QSemaphore>
#include <QSet>
#include <QtTest>

#include "asyncdriver.h"
#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "fakesessionstate.h"
#include "testmain.h"
#include "testutil.h"

using namespace FlySight;
using namespace FlySightTest;
using Synthetic::attr;

namespace {

using Names = QSet<DependencyKey>;
using Prepare = CalculationEngine::PrepareOutcome;

// Declaration order matters: a registry must outlive its engines.
struct World {
    CalculationRegistry registry;
    FakePreferenceProvider prefs;
    FakeSessionState state;
    CalculationEngine engine;
    QList<Names> broadcasts;    // what the invalidation listener received

    World()
        : engine(&state, &registry)
    {
        registry.setPreferenceProvider(&prefs);
        Synthetic::registerSharedWorld(registry);
        Synthetic::registerExplicitWorld(registry);
        engine.setInvalidationListener([this](const Names &names) { broadcasts.append(names); });
        // The usual starting point. Shared world: X = 3, p = 5.
        state.setAttribute("A", 1);
        state.setAttribute("B", 2);
        prefs.set("p", 5);
        state.setAttribute("EA_IN", 4);
        state.setAttribute("EB_IN", 10);
    }
};

/// prepare + compute + publish. A prepare that is not Ready yields the default
/// outcome (RefusedGone / SessionGone), which no caller expects.
PublishOutcome runAsync(CalculationEngine &engine, const CalculationId &id, ComputeMode mode,
                        CalculationProgress *progress = nullptr)
{
    Prepare prepared = engine.prepare(id);
    if (prepared.kind != Prepare::Kind::Ready || !prepared.ticket)
        return PublishOutcome();
    ComputedCalculation computed = computeOn(mode, *prepared.ticket, progress);
    return prepared.ticket->publish(std::move(computed));
}

// Test-local synchronization, so that an edit or a cancel provably happens
// while compute() is running. Real compute functions may not hold state; this
// one does because it is the test's instrument. (The rule against locks
// concerns the library.)
struct Gate {
    QSemaphore entered;
    QSemaphore proceed;
};

// gated: Explicit; input EA_IN; output G1 = EA_IN + 1. With a gate, signals
// "entered" and waits for "proceed" first. Observes cancellation after that.
CalculationDescriptor gated(const std::shared_ptr<Gate> &gate)
{
    CalculationDescriptor d;
    d.id = QStringLiteral("gated");
    d.title = QStringLiteral("Gated");
    d.policy = EvaluationPolicy::Explicit;
    d.inputs = {CalcInput::attribute("EA_IN")};
    d.outputs = {attr("G1")};
    d.compute = [gate](const EvaluationContext &ctx) {
        if (gate) {
            gate->entered.release();
            gate->proceed.acquire();
        }
        ctx.progress().throwIfCancelled();
        return CalculationResult().setAttribute("G1", ctx.attribute("EA_IN").toInt() + 1);
    };
    return d;
}

// What a compute function saw, for tests that cannot look into the opaque
// ComputedCalculation. Atomics: written by the worker, read after the join.
struct Seen {
    std::atomic<int> x{-1};
    std::atomic<int> in{-1};
    std::atomic<double> sum{-1.0};
    std::atomic<int> result{-1};
};

// capture: Explicit; inputs attr X (on demand: sum), attr EA_IN, meas S/m;
// output CAP = X + EA_IN + sum(S/m).
CalculationDescriptor capture(const std::shared_ptr<Seen> &seen)
{
    CalculationDescriptor d;
    d.id = QStringLiteral("capture");
    d.policy = EvaluationPolicy::Explicit;
    d.inputs = {CalcInput::attribute("X"), CalcInput::attribute("EA_IN"), CalcInput::measurement("S", "m")};
    d.outputs = {attr("CAP")};
    d.compute = [seen](const EvaluationContext &ctx) {
        const int x = ctx.attribute("X").toInt();
        const int in = ctx.attribute("EA_IN").toInt();
        double sum = 0.0;
        for (double v : ctx.measurement("S", "m"))
            sum += v;
        const int result = x + in + int(sum);
        seen->x = x;
        seen->in = in;
        seen->sum = sum;
        seen->result = result;
        return CalculationResult().setAttribute("CAP", result);
    };
    return d;
}

// trans: Explicit; inputs attr X (on demand, with fallbacks), pref p; TR = X + p.
CalculationDescriptor trans()
{
    CalculationDescriptor d;
    d.id = QStringLiteral("trans");
    d.policy = EvaluationPolicy::Explicit;
    d.inputs = {CalcInput::attribute("X"), CalcInput::preference("p")};
    d.outputs = {attr("TR")};
    d.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute("TR", ctx.attribute("X").toInt() + ctx.preference("p").toInt());
    };
    return d;
}

// thrower: Explicit; input EA_IN; outputs T1, T2; sets T1 and then throws.
CalculationDescriptor thrower()
{
    CalculationDescriptor d;
    d.id = QStringLiteral("thrower");
    d.policy = EvaluationPolicy::Explicit;
    d.inputs = {CalcInput::attribute("EA_IN")};
    d.outputs = {attr("T1"), attr("T2")};
    d.compute = [](const EvaluationContext &ctx) -> CalculationResult {
        CalculationResult r;
        r.setAttribute("T1", ctx.attribute("EA_IN"));   // already set when the exception leaves
        throw std::runtime_error("synthetic failure");
    };
    return d;
}

// explicitE / feedsE: the ring through an explicit calculation's own output,
// as in tst_calcengine_safety. explicitE: input EY, EX = EY + 1 (Explicit);
// feedsE: input EX, EY = EX * 2.
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

bool isNotRun(const std::optional<ResultStatus> &status)
{
    return !status || *status == ResultStatus::NotRequested;
}

} // namespace

class CalcEngineAsyncTest : public QObject {
    Q_OBJECT

private slots:
    void asyncMatchesSync_data() { addComputeModeRows(); }
    void asyncMatchesSync();
    void outputsAppearTogether_data() { addComputeModeRows(); }
    void outputsAppearTogether();
    void namesReadWhileUnrequestedAreInvalidated_data() { addComputeModeRows(); }
    void namesReadWhileUnrequestedAreInvalidated();
    void prepareCapturesInputs_data() { addComputeModeRows(); }
    void prepareCapturesInputs();
    void inputChangeWhileRunningRefuses_data() { addComputeModeRows(); }
    void inputChangeWhileRunningRefuses();
    void transitiveChangesRefuse_data();
    void transitiveChangesRefuse();
    void unrelatedChangeDoesNotRefuse_data() { addComputeModeRows(); }
    void unrelatedChangeDoesNotRefuse();
    void changeAfterPublicationDropsDependents_data() { addComputeModeRows(); }
    void changeAfterPublicationDropsDependents();
    void registrationRemovedRefuses_data() { addComputeModeRows(); }
    void registrationRemovedRefuses();
    void engineDestroyedRefuses_data() { addComputeModeRows(); }
    void engineDestroyedRefuses();
    void willBeRefusedReportsTheEnginesMarks_data();
    void willBeRefusedReportsTheEnginesMarks();
    void willBeRefusedIsAboutAPublishToCome();
    void computeNeedsNoEngine_data() { addComputeModeRows(); }
    void computeNeedsNoEngine();
    void cancelPublishesNothing_data() { addComputeModeRows(); }
    void cancelPublishesNothing();
    void progressTextReachesCaller_data() { addComputeModeRows(); }
    void progressTextReachesCaller();
    void resourceExhaustionIsNotCached_data() { addComputeModeRows(); }
    void resourceExhaustionIsNotCached();
    void ordinaryExceptionIsCachedFailure_data() { addComputeModeRows(); }
    void ordinaryExceptionIsCachedFailure();
    void undeclaredReadAndInvalidOutputMatchSync_data() { addComputeModeRows(); }
    void undeclaredReadAndInvalidOutputMatchSync();
    void missingInputPreparesNothing();
    void cycleThroughOwnOutputMatchesSync_data();
    void cycleThroughOwnOutputMatchesSync();
    void notExplicitIsRefused();
    void blockedPrepareNamesBlocker();
    void syncRequestInBetween_data() { addComputeModeRows(); }
    void syncRequestInBetween();
    void abandonedTicketLeavesNothing();
    void valueTypes();
};

// Sensor-fusion-jobs acceptance 7: asynchronous and synchronous requests produce identical
// results - every observable of the engine, not only the values.
void CalcEngineAsyncTest::asyncMatchesSync()
{
    QFETCH(int, mode);
    World sync, async;
    const QList<DependencyKey> names = Synthetic::explicitNames();
    const QList<DependencyKey> readFirst = {attr("EA1"), attr("DA"), attr("DDA"), attr("EA_DIAG"), attr("DB")};

    for (World *w : {&sync, &async}) {
        for (const DependencyKey &name : readFirst)
            QVERIFY(!w->engine.isAvailable(name));
        QCOMPARE(w->engine.totalRunCount(), 0);
    }

    // ---- A
    const CalculationEngine::RequestOutcome requested = sync.engine.request("expA");
    QVERIFY(requested.found);
    QCOMPARE(requested.status, ResultStatus::Ok);

    const PublishOutcome published = runAsync(async.engine, "expA", ComputeMode(mode));
    QCOMPARE(published.kind, PublishOutcome::Kind::Published);
    QCOMPARE(published.reason, PublishOutcome::Reason::None);
    QCOMPARE(published.status, ResultStatus::Ok);
    QCOMPARE(published.detail, QString());
    // DB was read too, but it hangs off expB, which is still not requested.
    QCOMPARE(published.invalidated, Names({attr("EA1"), attr("DA"), attr("DDA"), attr("EA_DIAG")}));
    QCOMPARE(published.invalidated, requested.invalidated);
    QVERIFY(async.broadcasts.isEmpty());        // returned to the caller, not broadcast
    QCOMPARE(async.engine.preparedCount(), 0);

    // ---- B, which consumes A's output
    const CalculationEngine::RequestOutcome requestedB = sync.engine.request("expB");
    const PublishOutcome publishedB = runAsync(async.engine, "expB", ComputeMode(mode));
    QCOMPARE(requestedB.status, ResultStatus::Ok);
    QCOMPARE(publishedB.kind, PublishOutcome::Kind::Published);
    QCOMPARE(publishedB.status, ResultStatus::Ok);
    // EB1 was never read directly, but resolving DB resolved (and cached) it.
    QCOMPARE(publishedB.invalidated, Names({attr("DB"), attr("EB1")}));
    QCOMPARE(publishedB.invalidated, requestedB.invalidated);

    // Literal values, then equality of everything observable.
    QCOMPARE(async.engine.attribute("EA1"), QVariant(5));
    QCOMPARE(async.engine.attribute("EA2"), QVariant(8));
    QCOMPARE(async.engine.attribute("EA_DIAG"), QVariant(QStringLiteral("ok")));
    QCOMPARE(async.engine.attribute("DA"), QVariant(105));
    QCOMPARE(async.engine.attribute("DDA"), QVariant(1105));
    QCOMPARE(async.engine.attribute("EB1"), QVariant(18));
    QCOMPARE(async.engine.attribute("DB"), QVariant(19));
    for (const DependencyKey &name : names) {
        QCOMPARE(async.engine.attribute(name.attributeKey), sync.engine.attribute(name.attributeKey));
        QVERIFY(CalculationEngine::sameValue(async.engine.evaluateFresh(name), sync.engine.evaluateFresh(name)));
    }
    for (const char *id : {"expA", "expB", "derivA", "derivA2", "derivB"}) {
        const GraphNode node = GraphNode::result(QString::fromLatin1(id));
        QCOMPARE(async.engine.dependenciesOf(node), sync.engine.dependenciesOf(node));
        QCOMPARE(async.engine.resultStatus(id), sync.engine.resultStatus(id));
        QCOMPARE(async.engine.resultDetail(id), sync.engine.resultDetail(id));
        QCOMPARE(async.engine.runCount(id), sync.engine.runCount(id));
    }
    QCOMPARE(async.engine.dependenciesOf(GraphNode::result("expA")),
             QSet<GraphNode>({GraphNode::resolution(attr("EA_IN"))}));
    QCOMPARE(async.engine.dependenciesOf(GraphNode::result("expB")),
             QSet<GraphNode>({GraphNode::resolution(attr("EA2")), GraphNode::resolution(attr("EB_IN"))}));
    QCOMPARE(async.engine.resultStatus("expA"), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(async.engine.runCount("expA"), 1);
    QCOMPARE(async.engine.runCount("expB"), 1);
    QCOMPARE(async.engine.totalRunCount(), sync.engine.totalRunCount());
    QCOMPARE(async.engine.undeclaredReadCount(), 0);
    QCOMPARE(async.engine.edgeCount(), sync.engine.edgeCount());
    QCOMPARE(async.engine.cachedNodeCount(), sync.engine.cachedNodeCount());
    QVERIFY(async.engine.verifyAgainstFresh(names).isEmpty());
    QVERIFY(sync.engine.verifyAgainstFresh(names).isEmpty());

    // A valid result is never recomputed, by either path.
    const Prepare again = async.engine.prepare("expA");
    QCOMPARE(again.kind, Prepare::Kind::AlreadyValid);
    QCOMPARE(again.status, ResultStatus::Ok);
    QVERIFY(!again.ticket);
    QVERIFY(again.invalidated.isEmpty());
    QCOMPARE(async.engine.request("expA").status, ResultStatus::Ok);
    QCOMPARE(async.engine.runCount("expA"), 1);
    QCOMPARE(async.engine.scopeDepth(), 0);
}

void CalcEngineAsyncTest::outputsAppearTogether()
{
    QFETCH(int, mode);
    World w;
    const QList<DependencyKey> outputs = {attr("EA1"), attr("EA2"), attr("EA_DIAG")};

    Prepare prepared = w.engine.prepare("expA");
    QCOMPARE(prepared.kind, Prepare::Kind::Ready);
    QVERIFY(prepared.ticket);
    QCOMPARE(prepared.ticket->registrationId(), QStringLiteral("expA"));
    QCOMPARE(prepared.ticket->instanceId(), QStringLiteral("expA"));
    QCOMPARE(prepared.ticket->title(), QStringLiteral("Explicit A"));
    QCOMPARE(w.engine.preparedCount(), 1);

    // An outstanding ticket changes nothing for readers: still "not requested".
    const auto stillUnrequested = [&w, &outputs]() {
        for (const DependencyKey &name : outputs) {
            if (w.engine.isAvailable(name) || w.engine.evaluateFresh(name).available)
                return false;
        }
        return w.engine.verifyAgainstFresh(outputs).isEmpty()
            && w.engine.resultStatus("expA") == std::optional<ResultStatus>(ResultStatus::NotRequested)
            && w.engine.runCount("expA") == 0;
    };
    QVERIFY(stillUnrequested());

    ComputedCalculation computed = computeOn(ComputeMode(mode), *prepared.ticket);
    QCOMPARE(computed.kind, ComputedCalculation::Kind::Completed);
    QVERIFY(stillUnrequested());                // computed, not published

    const PublishOutcome outcome = prepared.ticket->publish(std::move(computed));
    QCOMPARE(outcome.kind, PublishOutcome::Kind::Published);
    QCOMPARE(outcome.invalidated, Names({attr("EA1"), attr("EA2"), attr("EA_DIAG")}));
    for (const DependencyKey &name : outputs)
        QVERIFY(w.engine.isAvailable(name));
    QCOMPARE(w.engine.attribute("EA1"), QVariant(5));
    QCOMPARE(w.engine.attribute("EA2"), QVariant(8));
    QCOMPARE(w.engine.runCount("expA"), 1);
    QCOMPARE(w.engine.preparedCount(), 0);
    QVERIFY(w.engine.verifyAgainstFresh(outputs).isEmpty());
}

// Spec 7.1 step 3: the names read while the calculation was unrequested are
// invalidated by the publication - whenever they were read.
void CalcEngineAsyncTest::namesReadWhileUnrequestedAreInvalidated()
{
    QFETCH(int, mode);
    World w;

    // Read before prepare, and never again.
    QVERIFY(!w.engine.isAvailable(attr("EA1")));
    QVERIFY(!w.engine.isAvailable(attr("DA")));

    Prepare prepared = w.engine.prepare("expA");
    QCOMPARE(prepared.kind, Prepare::Kind::Ready);
    // prepare() dropped those answers (as request() does before it evaluates)...
    QCOMPARE(prepared.invalidated, Names({attr("EA1"), attr("DA")}));
    QCOMPARE(w.engine.cachedState(attr("EA1")), CalculationEngine::CachedState::NotCached);
    QCOMPARE(w.engine.cachedState(attr("DA")), CalculationEngine::CachedState::NotCached);

    // Read between prepare and publish.
    QVERIFY(!w.engine.isAvailable(attr("EA2")));
    QCOMPARE(w.engine.cachedState(attr("EA2")), CalculationEngine::CachedState::Unavailable);

    ComputedCalculation computed = computeOn(ComputeMode(mode), *prepared.ticket);
    const PublishOutcome outcome = prepared.ticket->publish(std::move(computed));
    QCOMPARE(outcome.kind, PublishOutcome::Kind::Published);
    // ...so publish() reports them from the ticket, together with the later one.
    QCOMPARE(outcome.invalidated, Names({attr("EA1"), attr("DA"), attr("EA2")}));
    QCOMPARE(w.engine.cachedState(attr("EA2")), CalculationEngine::CachedState::NotCached);
    QCOMPARE(w.engine.attribute("DA"), QVariant(105));
    QCOMPARE(w.engine.attribute("EA2"), QVariant(8));
    QVERIFY(w.engine.verifyAgainstFresh(Synthetic::explicitNames()).isEmpty());
}

// Spec 7.1 steps 1 and 2: prepare computes on-demand inputs and captures the
// values; later edits to the session do not reach the worker.
void CalcEngineAsyncTest::prepareCapturesInputs()
{
    QFETCH(int, mode);
    World w;
    const auto seen = std::make_shared<Seen>();
    QVERIFY(w.registry.registerCalculation(capture(seen)));
    w.state.setMeasurement("S", "m", {1.0, 2.0, 3.0});

    Prepare prepared = w.engine.prepare("capture");
    QCOMPARE(prepared.kind, Prepare::Kind::Ready);
    QCOMPARE(w.engine.runCount("sum"), 1);          // the on-demand input was computed by prepare
    QCOMPARE(w.engine.runCount("capture"), 0);
    QCOMPARE(seen->result.load(), -1);

    // Every input changes after the capture.
    w.state.setAttribute(w.engine, "A", 50);
    w.state.setAttribute(w.engine, "EA_IN", 60);
    w.state.setMeasurement(w.engine, "S", "m", {70.0, 80.0});

    ComputedCalculation computed = computeOn(ComputeMode(mode), *prepared.ticket);
    QCOMPARE(computed.kind, ComputedCalculation::Kind::Completed);
    QCOMPARE(seen->x.load(), 3);
    QCOMPARE(seen->in.load(), 4);
    QCOMPARE(seen->sum.load(), 6.0);
    QCOMPARE(seen->result.load(), 13);

    // And the engine refuses what was computed from them.
    const PublishOutcome outcome = prepared.ticket->publish(std::move(computed));
    QCOMPARE(outcome.kind, PublishOutcome::Kind::RefusedStale);
    QCOMPARE(outcome.reason, PublishOutcome::Reason::InputsChanged);
    QVERIFY(!w.engine.isAvailable(attr("CAP")));

    QCOMPARE(runAsync(w.engine, "capture", ComputeMode(mode)).kind, PublishOutcome::Kind::Published);
    QCOMPARE(w.engine.attribute("CAP"), QVariant(52 + 60 + 150));
    QCOMPARE(w.engine.runCount("capture"), 1);
}

// Sensor-fusion-jobs acceptance 8, first half: a declared input changes WHILE the calculation
// is running. The engine refuses the result from its own dependency records.
void CalcEngineAsyncTest::inputChangeWhileRunningRefuses()
{
    QFETCH(int, mode);
    const bool threaded = ComputeMode(mode) != ComputeMode::Inline;
    World w;
    const auto gate = threaded ? std::make_shared<Gate>() : std::shared_ptr<Gate>();
    QVERIFY(w.registry.registerCalculation(gated(gate)));
    QVERIFY(!w.engine.isAvailable(attr("G1")));

    Prepare prepared = w.engine.prepare("gated");
    QCOMPARE(prepared.kind, Prepare::Kind::Ready);

    Names changed;
    ComputedCalculation computed;
    if (threaded) {
        ComputeRun run(ComputeMode(mode), *prepared.ticket);
        gate->entered.acquire();                // the worker is inside compute()
        changed = w.state.setAttribute(w.engine, "EA_IN", 7);
        gate->proceed.release();
        computed = run.finish();
    } else {
        changed = w.state.setAttribute(w.engine, "EA_IN", 7);
        computed = computeOn(ComputeMode(mode), *prepared.ticket);
    }
    QCOMPARE(computed.kind, ComputedCalculation::Kind::Completed);  // it ran to the end, on the old input
    QVERIFY(changed.contains(attr("EA_IN")));
    QVERIFY(!changed.contains(attr("G1")));     // nothing was published, so nothing to drop

    const PublishOutcome outcome = prepared.ticket->publish(std::move(computed));
    QCOMPARE(outcome.kind, PublishOutcome::Kind::RefusedStale);
    QCOMPARE(outcome.reason, PublishOutcome::Reason::InputsChanged);
    QVERIFY(outcome.invalidated.isEmpty());
    QCOMPARE(outcome.status, ResultStatus::NotRequested);

    // Discarded whole: nothing published, no run counted, still requestable.
    QVERIFY(!w.engine.isAvailable(attr("G1")));
    QCOMPARE(w.engine.runCount("gated"), 0);
    QVERIFY(isNotRun(w.engine.resultStatus("gated")));
    QCOMPARE(w.engine.preparedCount(), 0);
    QVERIFY(w.engine.verifyAgainstFresh({attr("G1"), attr("EA_IN")}).isEmpty());

    // From here on every run goes straight through - the fresh evaluations of
    // the oracle included, which replay the request on the main thread.
    if (gate)
        gate->proceed.release(1000);
    const PublishOutcome second = runAsync(w.engine, "gated", ComputeMode(mode));
    QCOMPARE(second.kind, PublishOutcome::Kind::Published);
    QCOMPARE(w.engine.attribute("G1"), QVariant(8));
    QCOMPARE(w.engine.runCount("gated"), 1);
    QVERIFY(w.engine.verifyAgainstFresh({attr("G1")}).isEmpty());
}

void CalcEngineAsyncTest::transitiveChangesRefuse_data()
{
    QTest::addColumn<QString>("change");
    QTest::addColumn<int>("expected");
    QTest::newRow("on-demand intermediate") << "intermediate" << 9;     // A = 2: X = 4
    QTest::newRow("declared preference") << "preference" << 9;          // p = 6
    QTest::newRow("registry change of an input candidate") << "registry" << 4;  // sum removed: X = -1
    QTest::newRow("clear()") << "clear" << 8;
}

// Sensor-fusion-jobs acceptance 8: whatever reaches a prepared input through the dependency
// graph refuses the result - not only a direct edit of a declared attribute.
void CalcEngineAsyncTest::transitiveChangesRefuse()
{
    QFETCH(QString, change);
    QFETCH(int, expected);
    World w;
    QVERIFY(w.registry.registerCalculation(trans()));

    Prepare prepared = w.engine.prepare("trans");
    QCOMPARE(prepared.kind, Prepare::Kind::Ready);
    ComputedCalculation computed = computeOn(ComputeMode::StdThread, *prepared.ticket);

    if (change == QLatin1String("intermediate"))
        w.state.setAttribute(w.engine, "A", 2);
    else if (change == QLatin1String("preference"))
        w.prefs.set(w.registry, "p", 6);
    else if (change == QLatin1String("registry"))
        QVERIFY(w.registry.unregister("sum"));
    else
        w.engine.clear();

    const PublishOutcome outcome = prepared.ticket->publish(std::move(computed));
    QCOMPARE(outcome.kind, PublishOutcome::Kind::RefusedStale);
    QCOMPARE(outcome.reason, PublishOutcome::Reason::InputsChanged);
    QVERIFY(!w.engine.isAvailable(attr("TR")));
    QCOMPARE(w.engine.runCount("trans"), 0);

    const PublishOutcome second = runAsync(w.engine, "trans", ComputeMode::QtThread);
    QCOMPARE(second.kind, PublishOutcome::Kind::Published);
    QCOMPARE(w.engine.attribute("TR"), QVariant(expected));
    QCOMPARE(w.engine.runCount("trans"), 1);
    QVERIFY(w.engine.verifyAgainstFresh({attr("TR"), attr("X")}).isEmpty());
}

// Staleness follows the dependency records; it is not a generation counter.
void CalcEngineAsyncTest::unrelatedChangeDoesNotRefuse()
{
    QFETCH(int, mode);
    World w;
    QCOMPARE(w.engine.attribute("X"), QVariant(3));

    Prepare prepared = w.engine.prepare("expA");
    QCOMPARE(prepared.kind, Prepare::Kind::Ready);
    ComputedCalculation computed = computeOn(ComputeMode(mode), *prepared.ticket);

    // Edits, a preference change, and a registry change that touch nothing
    // expA's inputs depended on; and reads in between.
    w.state.setAttribute(w.engine, "EB_IN", 11);
    w.state.setAttribute(w.engine, "A", 9);
    w.prefs.set(w.registry, "p", 6);
    QVERIFY(w.registry.unregister("wAlt"));
    QCOMPARE(w.engine.attribute("X"), QVariant(11));
    QVERIFY(!w.engine.isAvailable(attr("EA1")));

    const PublishOutcome outcome = prepared.ticket->publish(std::move(computed));
    QCOMPARE(outcome.kind, PublishOutcome::Kind::Published);
    QCOMPARE(outcome.status, ResultStatus::Ok);
    QCOMPARE(w.engine.attribute("EA1"), QVariant(5));
    QVERIFY(w.engine.verifyAgainstFresh(Synthetic::explicitNames()).isEmpty());
}

// Sensor-fusion-jobs acceptance 8, second half: a declared input changes AFTER publication.
void CalcEngineAsyncTest::changeAfterPublicationDropsDependents()
{
    QFETCH(int, mode);
    World w;
    QCOMPARE(runAsync(w.engine, "expA", ComputeMode(mode)).kind, PublishOutcome::Kind::Published);
    QCOMPARE(runAsync(w.engine, "expB", ComputeMode(mode)).kind, PublishOutcome::Kind::Published);
    QCOMPARE(w.engine.attribute("DDA"), QVariant(1105));
    QCOMPARE(w.engine.attribute("DB"), QVariant(19));
    QCOMPARE(w.engine.attribute("EA_DIAG"), QVariant(QStringLiteral("ok")));

    const Names changed = w.state.setAttribute(w.engine, "EA_IN", 6);
    // The result and every dependent, through the second explicit calculation.
    QCOMPARE(changed, Names({attr("EA_IN"), attr("EA1"), attr("EA2"), attr("EA_DIAG"), attr("DA"), attr("DDA"),
                             attr("EB1"), attr("DB")}));
    for (const DependencyKey &name : Synthetic::explicitNames())
        QVERIFY2(!w.engine.isAvailable(name), qPrintable(describe(name)));
    QCOMPARE(w.engine.resultStatus("expA"), std::optional<ResultStatus>(ResultStatus::NotRequested));
    QCOMPARE(w.engine.runCount("expA"), 1);     // reads never start it again
    QVERIFY(w.engine.verifyAgainstFresh(Synthetic::explicitNames()).isEmpty());

    // Requestable again, A before B.
    Prepare b = w.engine.prepare("expB");
    QCOMPARE(b.kind, Prepare::Kind::Blocked);
    Prepare a = w.engine.prepare("expA");
    QCOMPARE(a.kind, Prepare::Kind::Ready);
    ComputedCalculation computed = computeOn(ComputeMode(mode), *a.ticket);
    QCOMPARE(a.ticket->publish(std::move(computed)).kind, PublishOutcome::Kind::Published);
    QCOMPARE(runAsync(w.engine, "expB", ComputeMode(mode)).kind, PublishOutcome::Kind::Published);
    QCOMPARE(w.engine.attribute("DDA"), QVariant(1107));
    QCOMPARE(w.engine.attribute("DB"), QVariant(23));
    QCOMPARE(w.engine.runCount("expA"), 2);
    QCOMPARE(w.engine.runCount("expB"), 2);
}

void CalcEngineAsyncTest::registrationRemovedRefuses()
{
    QFETCH(int, mode);
    for (const bool reregister : {false, true}) {
        World w;
        Prepare prepared = w.engine.prepare("expA");
        QCOMPARE(prepared.kind, Prepare::Kind::Ready);
        ComputedCalculation computed = computeOn(ComputeMode(mode), *prepared.ticket);

        QVERIFY(w.registry.unregister("expA"));
        if (reregister)
            QVERIFY(w.registry.registerCalculation(Synthetic::expA()));     // does not revive the ticket

        const PublishOutcome outcome = prepared.ticket->publish(std::move(computed));
        QCOMPARE(outcome.kind, PublishOutcome::Kind::RefusedGone);
        QCOMPARE(outcome.reason, PublishOutcome::Reason::RegistrationRemoved);
        QVERIFY(outcome.invalidated.isEmpty());
        QVERIFY(!w.engine.isAvailable(attr("EA1")));
        QCOMPARE(w.engine.runCount("expA"), 0);
        QCOMPARE(w.engine.preparedCount(), 0);

        const Prepare again = w.engine.prepare("expA");
        QCOMPARE(again.kind, reregister ? Prepare::Kind::Ready : Prepare::Kind::NotFound);
    }

    // Compute after the removal: the ticket keeps the function alive.
    World w;
    Prepare prepared = w.engine.prepare("expA");
    QCOMPARE(prepared.kind, Prepare::Kind::Ready);
    QVERIFY(w.registry.unregister("expA"));
    ComputedCalculation computed = computeOn(ComputeMode(mode), *prepared.ticket);
    QCOMPARE(computed.kind, ComputedCalculation::Kind::Completed);
    QCOMPARE(prepared.ticket->publish(std::move(computed)).reason, PublishOutcome::Reason::RegistrationRemoved);
}

void CalcEngineAsyncTest::engineDestroyedRefuses()
{
    QFETCH(int, mode);
    CalculationRegistry registry;
    Synthetic::registerExplicitWorld(registry);
    FakeSessionState state;
    state.setAttribute("EA_IN", 4);

    // Destroyed after compute, and destroyed before compute.
    for (const bool destroyFirst : {false, true}) {
        auto engine = std::make_unique<CalculationEngine>(&state, &registry);
        Prepare prepared = engine->prepare("expA");
        QCOMPARE(prepared.kind, Prepare::Kind::Ready);
        QCOMPARE(engine->preparedCount(), 1);

        ComputedCalculation computed;
        if (destroyFirst)
            engine.reset();
        computed = computeOn(ComputeMode(mode), *prepared.ticket);
        engine.reset();
        QCOMPARE(computed.kind, ComputedCalculation::Kind::Completed);
        QCOMPARE(registry.enrolledEngineCount(), 0);

        const PublishOutcome outcome = prepared.ticket->publish(std::move(computed));
        QCOMPARE(outcome.kind, PublishOutcome::Kind::RefusedGone);
        QCOMPARE(outcome.reason, PublishOutcome::Reason::SessionGone);
        QVERIFY(outcome.invalidated.isEmpty());
        prepared.ticket.reset();                // outlived the engine: must not call into it
    }

    // An unpublished ticket that outlives its engine.
    auto engine = std::make_unique<CalculationEngine>(&state, &registry);
    Prepare prepared = engine->prepare("expA");
    QCOMPARE(prepared.kind, Prepare::Kind::Ready);
    engine.reset();
    prepared.ticket.reset();
}

void CalcEngineAsyncTest::willBeRefusedReportsTheEnginesMarks_data()
{
    using Reason = PublishOutcome::Reason;
    QTest::addColumn<QString>("cause");
    QTest::addColumn<int>("reason");
    QTest::newRow("declared input") << "input" << int(Reason::InputsChanged);
    QTest::newRow("on-demand intermediate") << "intermediate" << int(Reason::InputsChanged);
    QTest::newRow("declared preference") << "preference" << int(Reason::InputsChanged);
    QTest::newRow("registry change of an input candidate") << "candidate" << int(Reason::InputsChanged);
    QTest::newRow("clear()") << "clear" << int(Reason::InputsChanged);
    QTest::newRow("registration removed") << "unregister" << int(Reason::RegistrationRemoved);
    QTest::newRow("registration removed and added again") << "reregister" << int(Reason::RegistrationRemoved);
    QTest::newRow("stale, then registration removed") << "staleThenGone" << int(Reason::RegistrationRemoved);
    QTest::newRow("engine destroyed") << "engine" << int(Reason::SessionGone);
}

// What a queue may ask while compute() is still running: is publish() already
// certain to refuse? The answer is the engine's own mark, for every cause of a
// refusal that can be known in advance, and it names the reason publish() gives.
void CalcEngineAsyncTest::willBeRefusedReportsTheEnginesMarks()
{
    QFETCH(QString, cause);
    QFETCH(int, reason);

    CalculationRegistry registry;
    FakePreferenceProvider prefs;
    registry.setPreferenceProvider(&prefs);
    Synthetic::registerSharedWorld(registry);
    QVERIFY(registry.registerCalculation(trans()));
    FakeSessionState state;
    state.setAttribute("A", 1);
    state.setAttribute("B", 2);
    prefs.set("p", 5);
    auto engine = std::make_unique<CalculationEngine>(&state, &registry);

    Prepare prepared = engine->prepare("trans");
    QCOMPARE(prepared.kind, Prepare::Kind::Ready);
    const PreparedCalculation &ticket = *prepared.ticket;

    // Healthy, and still healthy after reads and changes that reach none of
    // its inputs
    QVERIFY(!ticket.willBeRefused());
    QCOMPARE(ticket.refusalReason(), PublishOutcome::Reason::None);
    QVERIFY(!engine->isAvailable(attr("TR")));
    state.setAttribute(*engine, "UNRELATED", 1);
    prefs.set(registry, "q", 1);
    QVERIFY(!ticket.willBeRefused());

    // The run itself changes nothing either way: compute() neither reads nor
    // writes the mark
    ComputedCalculation computed = computeOn(ComputeMode::StdThread, *prepared.ticket);
    QVERIFY(!ticket.willBeRefused());

    if (cause == QLatin1String("input")) {
        state.setAttribute(*engine, "X", 40);       // stored: wins over the calculation
    } else if (cause == QLatin1String("intermediate")) {
        state.setAttribute(*engine, "A", 2);
    } else if (cause == QLatin1String("preference")) {
        prefs.set(registry, "p", 6);
    } else if (cause == QLatin1String("candidate")) {
        QVERIFY(registry.unregister("sum"));
    } else if (cause == QLatin1String("clear")) {
        engine->clear();
    } else if (cause == QLatin1String("unregister")) {
        QVERIFY(registry.unregister("trans"));
    } else if (cause == QLatin1String("reregister")) {
        QVERIFY(registry.unregister("trans"));
        QVERIFY(registry.registerCalculation(trans()));
    } else if (cause == QLatin1String("staleThenGone")) {
        state.setAttribute(*engine, "A", 2);
        QCOMPARE(ticket.refusalReason(), PublishOutcome::Reason::InputsChanged);
        QVERIFY(registry.unregister("trans"));      // "gone" outranks "stale"
    } else {
        engine.reset();
    }

    QVERIFY(ticket.willBeRefused());
    QCOMPARE(int(ticket.refusalReason()), reason);

    // It stays that way: undoing the change does not revive the ticket
    if (cause == QLatin1String("intermediate")) {
        state.setAttribute(*engine, "A", 1);
        QVERIFY(ticket.willBeRefused());
    }

    // And publish() says the same
    const PublishOutcome outcome = prepared.ticket->publish(std::move(computed));
    QVERIFY(outcome.kind == PublishOutcome::Kind::RefusedStale || outcome.kind == PublishOutcome::Kind::RefusedGone);
    QCOMPARE(int(outcome.reason), reason);

    // Spent: the question is about a publish() to come
    QVERIFY(!ticket.willBeRefused());
    QCOMPARE(ticket.refusalReason(), PublishOutcome::Reason::None);
}

// False never means "was published", and true never outlives publish():
// willBeRefused() is false after a successful publication (and stays false when
// the published result is invalidated later), and false after a refusal it did
// not predict. A synchronous request() in between is such a refusal: only
// publish() decides it.
void CalcEngineAsyncTest::willBeRefusedIsAboutAPublishToCome()
{
    {
        World w;
        Prepare prepared = w.engine.prepare("expA");
        QCOMPARE(prepared.kind, Prepare::Kind::Ready);
        ComputedCalculation computed = computeOn(ComputeMode::StdThread, *prepared.ticket);
        QVERIFY(!prepared.ticket->willBeRefused());
        QCOMPARE(prepared.ticket->publish(std::move(computed)).kind, PublishOutcome::Kind::Published);
        QVERIFY(!prepared.ticket->willBeRefused());

        // The published result goes stale; the spent ticket has nothing to say
        w.state.setAttribute(w.engine, "EA_IN", 6);
        QVERIFY(!w.engine.isAvailable(attr("EA1")));
        QVERIFY(!prepared.ticket->willBeRefused());
        QCOMPARE(prepared.ticket->refusalReason(), PublishOutcome::Reason::None);
    }
    {
        World w;
        Prepare prepared = w.engine.prepare("expA");
        QCOMPARE(prepared.kind, Prepare::Kind::Ready);
        ComputedCalculation computed = computeOn(ComputeMode::StdThread, *prepared.ticket);
        QCOMPARE(w.engine.request("expA").status, ResultStatus::Ok);
        QVERIFY(!prepared.ticket->willBeRefused());     // not a mark of the engine's
        QCOMPARE(prepared.ticket->publish(std::move(computed)).reason, PublishOutcome::Reason::AlreadyPublished);
        QVERIFY(!prepared.ticket->willBeRefused());
    }
}

// The narrow rule of spec 7.1: compute() sees the captured inputs and nothing
// else, so it works when there is nothing else left.
void CalcEngineAsyncTest::computeNeedsNoEngine()
{
    QFETCH(int, mode);
    const auto seen = std::make_shared<Seen>();
    auto registry = std::make_unique<CalculationRegistry>();
    auto prefs = std::make_unique<FakePreferenceProvider>();
    auto state = std::make_unique<FakeSessionState>();
    registry->setPreferenceProvider(prefs.get());
    Synthetic::registerSharedWorld(*registry);
    QVERIFY(registry->registerCalculation(capture(seen)));
    state->setAttribute("A", 1);
    state->setAttribute("B", 2);
    state->setAttribute("EA_IN", 4);
    state->setMeasurement("S", "m", {1.0, 2.0, 3.0});
    auto engine = std::make_unique<CalculationEngine>(state.get(), registry.get());

    Prepare prepared = engine->prepare("capture");
    QCOMPARE(prepared.kind, Prepare::Kind::Ready);

    engine.reset();
    state.reset();
    registry.reset();
    prefs.reset();

    ComputedCalculation computed = computeOn(ComputeMode(mode), *prepared.ticket);
    QCOMPARE(computed.kind, ComputedCalculation::Kind::Completed);
    QCOMPARE(seen->x.load(), 3);
    QCOMPARE(seen->in.load(), 4);
    QCOMPARE(seen->sum.load(), 6.0);
    QCOMPARE(seen->result.load(), 13);
    QCOMPARE(prepared.ticket->title(), QStringLiteral("capture"));  // no title: the instance id

    const PublishOutcome outcome = prepared.ticket->publish(std::move(computed));
    QCOMPARE(outcome.kind, PublishOutcome::Kind::RefusedGone);
    QCOMPARE(outcome.reason, PublishOutcome::Reason::SessionGone);
}

// Spec 7.1: cancellation publishes nothing; the calculation remains "not
// requested", as if it had never been asked.
void CalcEngineAsyncTest::cancelPublishesNothing()
{
    QFETCH(int, mode);
    const bool threaded = ComputeMode(mode) != ComputeMode::Inline;
    World w;
    const auto gate = threaded ? std::make_shared<Gate>() : std::shared_ptr<Gate>();
    QVERIFY(w.registry.registerCalculation(gated(gate)));
    QVERIFY(!w.engine.isAvailable(attr("G1")));

    Prepare prepared = w.engine.prepare("gated");
    QCOMPARE(prepared.kind, Prepare::Kind::Ready);
    QCOMPARE(prepared.invalidated, Names({attr("G1")}));

    RecordingProgress progress;
    ComputedCalculation computed;
    if (threaded) {
        ComputeRun run(ComputeMode(mode), *prepared.ticket, &progress);
        gate->entered.acquire();                // cancelled while compute() is running
        progress.cancel();
        gate->proceed.release();
        computed = run.finish();
    } else {
        progress.cancel();
        computed = computeOn(ComputeMode(mode), *prepared.ticket, &progress);
    }
    QCOMPARE(computed.kind, ComputedCalculation::Kind::Cancelled);

    const PublishOutcome outcome = prepared.ticket->publish(std::move(computed));
    QCOMPARE(outcome.kind, PublishOutcome::Kind::Discarded);
    QCOMPARE(outcome.reason, PublishOutcome::Reason::Cancelled);
    QVERIFY(outcome.invalidated.isEmpty());

    // Nothing published, nothing cached, no run counted.
    QVERIFY(!w.engine.isAvailable(attr("G1")));
    QVERIFY(isNotRun(w.engine.resultStatus("gated")));
    QCOMPARE(w.engine.runCount("gated"), 0);
    QCOMPARE(w.engine.preparedCount(), 0);
    QVERIFY(w.engine.verifyAgainstFresh({attr("G1")}).isEmpty());

    // Requestable again; without a cancel request it completes. The
    // synchronous path has no facility: there it is never cancelled.
    if (gate)
        gate->proceed.release(1000);            // later runs go straight through
    RecordingProgress notCancelled;
    const PublishOutcome second = runAsync(w.engine, "gated", ComputeMode(mode), &notCancelled);
    QCOMPARE(second.kind, PublishOutcome::Kind::Published);
    QCOMPARE(w.engine.attribute("G1"), QVariant(5));

    w.state.setAttribute(w.engine, "EA_IN", 5);
    QCOMPARE(w.engine.request("gated").status, ResultStatus::Ok);
    QCOMPARE(w.engine.attribute("G1"), QVariant(6));
}

void CalcEngineAsyncTest::progressTextReachesCaller()
{
    QFETCH(int, mode);
    World w;
    CalculationDescriptor d;
    d.id = QStringLiteral("talker");
    d.policy = EvaluationPolicy::Explicit;
    d.inputs = {CalcInput::attribute("EA_IN")};
    d.outputs = {attr("TALK")};
    d.compute = [](const EvaluationContext &ctx) {
        ctx.progress().report(QStringLiteral("building"));
        ctx.progress().report(QStringLiteral("iteration 1"));
        const bool cancelled = ctx.progress().isCancelled();
        ctx.progress().report(cancelled ? QStringLiteral("cancelled") : QStringLiteral("done"));
        return CalculationResult().setAttribute("TALK", ctx.attribute("EA_IN").toInt() * 3);
    };
    QVERIFY(w.registry.registerCalculation(d));

    RecordingProgress progress;
    const PublishOutcome outcome = runAsync(w.engine, "talker", ComputeMode(mode), &progress);
    QCOMPARE(outcome.kind, PublishOutcome::Kind::Published);
    QCOMPARE(progress.texts(), QStringList({"building", "iteration 1", "done"}));
    QCOMPARE(w.engine.attribute("TALK"), QVariant(12));

    // No facility given, and the synchronous path: text is dropped, the result is the same.
    w.state.setAttribute(w.engine, "EA_IN", 5);
    QCOMPARE(runAsync(w.engine, "talker", ComputeMode(mode)).kind, PublishOutcome::Kind::Published);
    QCOMPARE(w.engine.attribute("TALK"), QVariant(15));
    w.state.setAttribute(w.engine, "EA_IN", 6);
    QCOMPARE(w.engine.request("talker").status, ResultStatus::Ok);
    QCOMPARE(w.engine.attribute("TALK"), QVariant(18));
}

// Spec 8.2 / section 12: a failure that is not a function of the inputs is
// never cached.
void CalcEngineAsyncTest::resourceExhaustionIsNotCached()
{
    QFETCH(int, mode);
    World w;
    // Real compute functions may not hold state. This one misbehaves on
    // purpose: memory "runs out" on the first call only.
    const auto calls = std::make_shared<std::atomic<int>>(0);
    CalculationDescriptor d;
    d.id = QStringLiteral("hungry");
    d.policy = EvaluationPolicy::Explicit;
    d.inputs = {CalcInput::attribute("EA_IN")};
    d.outputs = {attr("HUNGRY")};
    d.compute = [calls](const EvaluationContext &ctx) {
        if (calls->fetch_add(1) == 0)
            throw std::bad_alloc();
        return CalculationResult().setAttribute("HUNGRY", ctx.attribute("EA_IN").toInt() + 1);
    };
    QVERIFY(w.registry.registerCalculation(d));

    WarningCapture warnings;
    Prepare prepared = w.engine.prepare("hungry");
    QCOMPARE(prepared.kind, Prepare::Kind::Ready);
    ComputedCalculation computed = computeOn(ComputeMode(mode), *prepared.ticket);
    QCOMPARE(computed.kind, ComputedCalculation::Kind::ResourceExhausted);
    QVERIFY(computed.failureText.isEmpty());

    const PublishOutcome outcome = prepared.ticket->publish(std::move(computed));
    QCOMPARE(outcome.kind, PublishOutcome::Kind::Discarded);
    QCOMPARE(outcome.reason, PublishOutcome::Reason::ResourceExhausted);
    QVERIFY(!w.engine.isAvailable(attr("HUNGRY")));
    QVERIFY(isNotRun(w.engine.resultStatus("hungry")));     // in particular not Failed
    QCOMPARE(w.engine.runCount("hungry"), 0);
    QCOMPARE(warnings.count(), 0);

    // So it can be requested again, and this time there is memory.
    const PublishOutcome second = runAsync(w.engine, "hungry", ComputeMode(mode));
    QCOMPARE(second.kind, PublishOutcome::Kind::Published);
    QCOMPARE(second.status, ResultStatus::Ok);
    QCOMPARE(w.engine.attribute("HUNGRY"), QVariant(5));
    QCOMPARE(w.engine.runCount("hungry"), 1);
}

// An exception other than resource exhaustion is a calculation failure in the
// engine's existing sense: a function of the inputs, published and cached.
void CalcEngineAsyncTest::ordinaryExceptionIsCachedFailure()
{
    QFETCH(int, mode);
    World sync, async;
    QVERIFY(sync.registry.registerCalculation(thrower()));
    QVERIFY(async.registry.registerCalculation(thrower()));

    WarningCapture warnings;

    // Synchronous half
    QCOMPARE(sync.engine.request("thrower").status, ResultStatus::Failed);
    QCOMPARE(sync.engine.resultDetail("thrower"), QStringLiteral("synthetic failure"));
    const QStringList syncWarnings = warnings.messages();
    QCOMPARE(syncWarnings, QStringList({"Calculation thrower failed: synthetic failure"}));

    // Asynchronous half
    Prepare prepared = async.engine.prepare("thrower");
    QCOMPARE(prepared.kind, Prepare::Kind::Ready);
    ComputedCalculation computed = computeOn(ComputeMode(mode), *prepared.ticket);
    QCOMPARE(computed.kind, ComputedCalculation::Kind::Failed);
    QCOMPARE(computed.failureText, QStringLiteral("synthetic failure"));
    QCOMPARE(warnings.count(), 1);              // compute() logs nothing

    const PublishOutcome outcome = prepared.ticket->publish(std::move(computed));
    QCOMPARE(outcome.kind, PublishOutcome::Kind::Published);
    QCOMPARE(outcome.status, ResultStatus::Failed);
    QCOMPARE(outcome.detail, QStringLiteral("synthetic failure"));
    QCOMPARE(warnings.messages(), syncWarnings + syncWarnings);     // the same warning, at publish

    QCOMPARE(async.engine.resultStatus("thrower"), std::optional<ResultStatus>(ResultStatus::Failed));
    QCOMPARE(async.engine.resultDetail("thrower"), QStringLiteral("synthetic failure"));
    QVERIFY(!async.engine.isAvailable(attr("T1")));     // no partial result
    QVERIFY(!async.engine.isAvailable(attr("T2")));
    QCOMPARE(async.engine.runCount("thrower"), 1);
    QCOMPARE(async.engine.runCount("thrower"), sync.engine.runCount("thrower"));
    QCOMPARE(async.engine.dependenciesOf(GraphNode::result("thrower")),
             sync.engine.dependenciesOf(GraphNode::result("thrower")));

    // Cached like any other result: asking again runs nothing.
    const Prepare again = async.engine.prepare("thrower");
    QCOMPARE(again.kind, Prepare::Kind::AlreadyValid);
    QCOMPARE(again.status, ResultStatus::Failed);
    QCOMPARE(async.engine.runCount("thrower"), 1);
    QVERIFY(async.engine.verifyAgainstFresh({attr("T1"), attr("T2")}).isEmpty());
}

void CalcEngineAsyncTest::undeclaredReadAndInvalidOutputMatchSync()
{
    QFETCH(int, mode);

    CalculationDescriptor sneaky;
    sneaky.id = QStringLiteral("sneaky");
    sneaky.policy = EvaluationPolicy::Explicit;
    sneaky.inputs = {CalcInput::attribute("EA_IN")};
    sneaky.outputs = {attr("SN")};
    sneaky.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute("SN", ctx.attribute("EA_IN").toInt()
                                                          + ctx.attribute("EB_IN").toInt());
    };

    CalculationDescriptor rogue;
    rogue.id = QStringLiteral("rogue");
    rogue.policy = EvaluationPolicy::Explicit;
    rogue.inputs = {CalcInput::attribute("EA_IN")};
    rogue.outputs = {attr("RG")};
    rogue.compute = [](const EvaluationContext &) {
        return CalculationResult().setAttribute("RG", 1).setAttribute("NOT_DECLARED", 2);
    };

    World sync, async;
    for (World *w : {&sync, &async}) {
        QVERIFY(w->registry.registerCalculation(sneaky));
        QVERIFY(w->registry.registerCalculation(rogue));
    }

    WarningCapture warnings;
    QCOMPARE(sync.engine.request("sneaky").status, ResultStatus::UndeclaredRead);
    QCOMPARE(sync.engine.request("rogue").status, ResultStatus::InvalidOutput);
    const QStringList syncWarnings = warnings.messages();
    QCOMPARE(syncWarnings,
             QStringList({"Calculation sneaky read an undeclared input: attribute(EB_IN) - its result is discarded",
                          "Calculation rogue set an output it did not declare: NOT_DECLARED - its result is discarded"}));

    const PublishOutcome sn = runAsync(async.engine, "sneaky", ComputeMode(mode));
    QCOMPARE(sn.kind, PublishOutcome::Kind::Published);
    QCOMPARE(sn.status, ResultStatus::UndeclaredRead);
    const PublishOutcome rg = runAsync(async.engine, "rogue", ComputeMode(mode));
    QCOMPARE(rg.kind, PublishOutcome::Kind::Published);
    QCOMPARE(rg.status, ResultStatus::InvalidOutput);
    QCOMPARE(warnings.messages(), syncWarnings + syncWarnings);     // same texts, from publish()

    QCOMPARE(async.engine.undeclaredReadCount(), 1);
    QCOMPARE(async.engine.undeclaredReadCount(), sync.engine.undeclaredReadCount());
    QCOMPARE(async.engine.lastUndeclaredRead().first, QStringLiteral("sneaky"));
    QVERIFY(async.engine.lastUndeclaredRead().second == CalcInput::attribute("EB_IN"));
    QVERIFY(async.engine.lastUndeclaredRead() == sync.engine.lastUndeclaredRead());
    for (const char *id : {"sneaky", "rogue"}) {
        QCOMPARE(async.engine.resultStatus(id), sync.engine.resultStatus(id));
        QCOMPARE(async.engine.runCount(id), 1);
        QCOMPARE(sync.engine.runCount(id), 1);
    }
    QVERIFY(!async.engine.isAvailable(attr("SN")));
    QVERIFY(!async.engine.isAvailable(attr("RG")));
    QVERIFY(async.engine.verifyAgainstFresh({attr("SN"), attr("RG")}).isEmpty());
}

// Spec 7.1 step 1: an unavailable input means "missing input" and nothing to
// run. prepare() leaves the cache exactly as request() does.
void CalcEngineAsyncTest::missingInputPreparesNothing()
{
    World sync, async;
    for (World *w : {&sync, &async}) {
        w->state.removeAttribute("EA_IN");
        QVERIFY(!w->engine.isAvailable(attr("EA1")));
        QVERIFY(!w->engine.isAvailable(attr("DA")));
    }

    const CalculationEngine::RequestOutcome requested = sync.engine.request("expA");
    Prepare prepared = async.engine.prepare("expA");
    QCOMPARE(prepared.kind, Prepare::Kind::NothingToRun);   // never Blocked: no request could help
    QCOMPARE(prepared.status, ResultStatus::MissingInput);
    QVERIFY(!prepared.ticket);
    QVERIFY(prepared.blockers.isEmpty());
    QCOMPARE(requested.status, ResultStatus::MissingInput);
    QCOMPARE(prepared.invalidated, Names({attr("EA1"), attr("DA")}));
    QCOMPARE(prepared.invalidated, requested.invalidated);

    QCOMPARE(async.engine.resultStatus("expA"), std::optional<ResultStatus>(ResultStatus::MissingInput));
    QCOMPARE(async.engine.resultStatus("expA"), sync.engine.resultStatus("expA"));
    QCOMPARE(async.engine.dependenciesOf(GraphNode::result("expA")),
             QSet<GraphNode>({GraphNode::resolution(attr("EA_IN"))}));
    QCOMPARE(async.engine.dependenciesOf(GraphNode::result("expA")),
             sync.engine.dependenciesOf(GraphNode::result("expA")));
    QCOMPARE(async.engine.edgeCount(), sync.engine.edgeCount());
    QCOMPARE(async.engine.cachedNodeCount(), sync.engine.cachedNodeCount());
    QCOMPARE(async.engine.preparedCount(), 0);
    QCOMPARE(async.engine.totalRunCount(), 0);
    QVERIFY(async.engine.verifyAgainstFresh(Synthetic::explicitNames()).isEmpty());

    // A valid (negative) result: not re-evaluated, by either path.
    QCOMPARE(async.engine.prepare("expA").kind, Prepare::Kind::AlreadyValid);
    QCOMPARE(async.engine.prepare("expA").status, ResultStatus::MissingInput);

    // The input arrives: the result is dropped, and now there is something to run.
    QVERIFY(async.state.setAttribute(async.engine, "EA_IN", 4).contains(attr("EA_IN")));
    QCOMPARE(async.engine.prepare("expA").kind, Prepare::Kind::Ready);
}

void CalcEngineAsyncTest::cycleThroughOwnOutputMatchesSync_data()
{
    QTest::addColumn<bool>("readFirst");
    QTest::newRow("nothing cached") << false;
    QTest::newRow("not-requested answers cached") << true;
}

// prepare() meets a ring through the calculation's own output exactly as
// request() does (tst_calcengine_safety::requestThroughOwnOutputIsCycle).
void CalcEngineAsyncTest::cycleThroughOwnOutputMatchesSync()
{
    QFETCH(bool, readFirst);
    struct Ring {
        CalculationRegistry registry;
        FakeSessionState state;
        CalculationEngine engine;
        Ring() : engine(&state, &registry)
        {
            registry.registerCalculation(explicitE());
            registry.registerCalculation(feedsE());
        }
    } sync, async;

    WarningCapture warnings;    // the cycle is reported, once per world
    for (Ring *r : {&sync, &async}) {
        if (readFirst) {
            QVERIFY(!r->engine.attribute("EX").isValid());
            QVERIFY(!r->engine.attribute("EY").isValid());
            QCOMPARE(r->engine.cycleCount(), 0);
        }
    }

    const CalculationEngine::RequestOutcome requested = sync.engine.request("explicitE");
    Prepare prepared = async.engine.prepare("explicitE");
    QCOMPARE(prepared.kind, Prepare::Kind::NothingToRun);
    QCOMPARE(prepared.status, ResultStatus::Cycle);
    QVERIFY(!prepared.ticket);
    QCOMPARE(requested.status, ResultStatus::Cycle);
    QCOMPARE(prepared.invalidated, readFirst ? Names({attr("EX"), attr("EY")}) : Names());
    QCOMPARE(prepared.invalidated, requested.invalidated);

    QCOMPARE(async.engine.cycleCount(), 1);
    QCOMPARE(async.engine.cycleCount(), sync.engine.cycleCount());
    QCOMPARE(async.engine.lastCyclePath(),
             QList<GraphNode>({GraphNode::result("explicitE"), GraphNode::resolution(attr("EY")),
                               GraphNode::result("feedsE"), GraphNode::resolution(attr("EX")),
                               GraphNode::result("explicitE")}));
    QCOMPARE(async.engine.lastCyclePath(), sync.engine.lastCyclePath());
    QCOMPARE(async.engine.totalRunCount(), 0);
    QCOMPARE(async.engine.scopeDepth(), 0);
    QCOMPARE(async.engine.resultStatus("explicitE"), std::optional<ResultStatus>(ResultStatus::Cycle));
    QCOMPARE(async.engine.resultStatus("feedsE"), sync.engine.resultStatus("feedsE"));
    for (const char *name : {"EX", "EY"}) {
        QCOMPARE(async.engine.cachedState(attr(name)), sync.engine.cachedState(attr(name)));
        QVERIFY(!async.engine.attribute(name).isValid());
        QVERIFY(!sync.engine.attribute(name).isValid());
        QCOMPARE(async.engine.cachedState(attr(name)), CalculationEngine::CachedState::Unavailable);
        QCOMPARE(async.engine.cachedState(attr(name)), sync.engine.cachedState(attr(name)));
    }
    QCOMPARE(async.engine.edgeCount(), sync.engine.edgeCount());
    QVERIFY(async.engine.verifyAgainstFresh({attr("EX"), attr("EY")}).isEmpty());

    // The cycle answer is a valid result.
    QCOMPARE(async.engine.prepare("explicitE").kind, Prepare::Kind::AlreadyValid);
    QCOMPARE(async.engine.cycleCount(), sync.engine.cycleCount());

    // Storing EY breaks the ring; then there is something to run.
    async.state.setAttribute(async.engine, "EY", 5);
    QCOMPARE(runAsync(async.engine, "explicitE", ComputeMode::StdThread).kind, PublishOutcome::Kind::Published);
    QCOMPARE(async.engine.attribute("EX"), QVariant(6));
    QCOMPARE(async.engine.runCount("explicitE"), 1);
}

// Only explicit calculations may be prepared, which keeps every on-demand (and
// plugin) compute function on the main thread by construction.
void CalcEngineAsyncTest::notExplicitIsRefused()
{
    World w;
    const Prepare onDemand = w.engine.prepare("sum");
    QCOMPARE(onDemand.kind, Prepare::Kind::NotExplicit);
    QVERIFY(!onDemand.ticket);
    const Prepare family = w.engine.prepare("neg", attr("neg:A"));
    QCOMPARE(family.kind, Prepare::Kind::NotExplicit);
    const Prepare unknown = w.engine.prepare("nope");
    QCOMPARE(unknown.kind, Prepare::Kind::NotFound);
    QCOMPARE(w.engine.prepare("neg").kind, Prepare::Kind::NotFound);     // a family needs a name
    QCOMPARE(w.engine.prepare("neg", attr("notOfThisFamily")).kind, Prepare::Kind::NotFound);

    // Nothing changed.
    QCOMPARE(w.engine.cachedNodeCount(), 0);
    QCOMPARE(w.engine.edgeCount(), 0);
    QCOMPARE(w.engine.totalRunCount(), 0);
    QCOMPARE(w.engine.preparedCount(), 0);
    QCOMPARE(w.state.readCount(), 0);

    // request() still accepts any policy.
    QCOMPARE(w.engine.request("sum").status, ResultStatus::Ok);
}

// Spec 7.1 step 1, last sentence: preparing never runs an explicit
// calculation; an input that is an unrequested explicit output makes this one
// blocked, and the outcome names what to ask for first.
void CalcEngineAsyncTest::blockedPrepareNamesBlocker()
{
    World w;
    Prepare prepared = w.engine.prepare("expB");
    QCOMPARE(prepared.kind, Prepare::Kind::Blocked);
    QCOMPARE(prepared.status, ResultStatus::MissingInput);
    QVERIFY(!prepared.ticket);
    QCOMPARE(prepared.blockers.size(), 1);
    QCOMPARE(prepared.blockers.first().registrationId, QStringLiteral("expA"));
    QCOMPARE(prepared.blockers.first().instanceId, QStringLiteral("expA"));
    QCOMPARE(prepared.blockers.first().title, QStringLiteral("Explicit A"));
    QVERIFY(isEmptyName(prepared.blockers.first().instanceOutput));
    QCOMPARE(w.engine.runCount("expA"), 0);
    QCOMPARE(w.engine.totalRunCount(), 0);
    QCOMPARE(w.engine.resultStatus("expB"), std::optional<ResultStatus>(ResultStatus::MissingInput));
    QVERIFY(w.engine.verifyAgainstFresh(Synthetic::explicitNames()).isEmpty());

    // The blocker round-trips into prepare(); once it has published, B is ready.
    const CalculationBlocker blocker = prepared.blockers.first();
    Prepare a = w.engine.prepare(blocker.registrationId, blocker.instanceOutput);
    QCOMPARE(a.kind, Prepare::Kind::Ready);
    ComputedCalculation computed = computeOn(ComputeMode::StdThread, *a.ticket);
    QCOMPARE(a.ticket->publish(std::move(computed)).kind, PublishOutcome::Kind::Published);
    QCOMPARE(w.engine.prepare("expB").kind, Prepare::Kind::Ready);
}

void CalcEngineAsyncTest::syncRequestInBetween()
{
    QFETCH(int, mode);
    World w;
    QVERIFY(!w.engine.isAvailable(attr("EA1")));

    Prepare prepared = w.engine.prepare("expA");
    QCOMPARE(prepared.kind, Prepare::Kind::Ready);
    ComputedCalculation computed = computeOn(ComputeMode(mode), *prepared.ticket);

    QCOMPARE(w.engine.request("expA").status, ResultStatus::Ok);
    QCOMPARE(w.engine.attribute("EA1"), QVariant(5));
    QCOMPARE(w.engine.runCount("expA"), 1);

    const PublishOutcome outcome = prepared.ticket->publish(std::move(computed));
    QCOMPARE(outcome.kind, PublishOutcome::Kind::RefusedStale);
    QCOMPARE(outcome.reason, PublishOutcome::Reason::AlreadyPublished);
    QVERIFY(outcome.invalidated.isEmpty());

    // The synchronous result stays in place, untouched.
    QCOMPARE(w.engine.cachedState(attr("EA1")), CalculationEngine::CachedState::Available);
    QCOMPARE(w.engine.attribute("EA1"), QVariant(5));
    QCOMPARE(w.engine.runCount("expA"), 1);
    QCOMPARE(w.engine.preparedCount(), 0);
    QVERIFY(w.engine.verifyAgainstFresh(Synthetic::explicitNames()).isEmpty());
}

// "As if it had never been asked": an abandoned request leaves no trace in the
// dependency graph, however often it happens.
void CalcEngineAsyncTest::abandonedTicketLeavesNothing()
{
    World w;
    QCOMPARE(w.engine.attribute("EA_IN"), QVariant(4));     // the input's resolution is cached
    const int edges = w.engine.edgeCount();
    const int nodes = w.engine.cachedNodeCount();

    {
        Prepare prepared = w.engine.prepare("expA");
        QCOMPARE(prepared.kind, Prepare::Kind::Ready);
        QCOMPARE(w.engine.preparedCount(), 1);
        QCOMPARE(w.engine.edgeCount(), edges + 1);          // the ticket's edge to its input
        QCOMPARE(w.engine.cachedNodeCount(), nodes);        // a ticket is not a cached node
    }
    QCOMPARE(w.engine.preparedCount(), 0);
    QCOMPARE(w.engine.edgeCount(), edges);
    QCOMPARE(w.engine.cachedNodeCount(), nodes);

    for (int i = 0; i < 1000; ++i) {
        Prepare prepared = w.engine.prepare("expA");
        QCOMPARE(prepared.kind, Prepare::Kind::Ready);
        if (i % 2 == 0) {
            // Computed, never published.
            QCOMPARE(prepared.ticket->compute().kind, ComputedCalculation::Kind::Completed);
        }
    }
    QCOMPARE(w.engine.preparedCount(), 0);
    QCOMPARE(w.engine.edgeCount(), edges);
    QCOMPARE(w.engine.cachedNodeCount(), nodes);
    QCOMPARE(w.engine.runCount("expA"), 0);
    QVERIFY(isNotRun(w.engine.resultStatus("expA")));
    QVERIFY(w.engine.verifyAgainstFresh(Synthetic::explicitNames()).isEmpty());

    // Several tickets at once, stale ones included, withdrawn in any order.
    Prepare first = w.engine.prepare("expA");
    Prepare second = w.engine.prepare("expA");
    QCOMPARE(w.engine.preparedCount(), 2);
    w.state.setAttribute(w.engine, "EA_IN", 5);             // both are stale now, and still outstanding
    QCOMPARE(w.engine.preparedCount(), 2);
    Prepare third = w.engine.prepare("expA");
    first.ticket.reset();
    QCOMPARE(w.engine.preparedCount(), 2);
    QCOMPARE(second.ticket->publish(second.ticket->compute()).reason, PublishOutcome::Reason::InputsChanged);
    QCOMPARE(third.ticket->publish(third.ticket->compute()).kind, PublishOutcome::Kind::Published);
    QCOMPARE(w.engine.preparedCount(), 0);
    QCOMPARE(w.engine.attribute("EA1"), QVariant(6));
}

// The small value-level vocabulary of the phase.
void CalcEngineAsyncTest::valueTypes()
{
    QCOMPARE(CalculationResult().reason(), QString());
    QCOMPARE(CalculationResult().setReason("x").reason(), QStringLiteral("x"));

    CalculationProgress &none = CalculationProgress::none();
    QVERIFY(!none.isCancelled());
    none.report(QStringLiteral("dropped"));
    none.throwIfCancelled();                    // does not throw

    RecordingProgress progress;
    progress.throwIfCancelled();
    progress.cancel();
    QVERIFY_THROWS_EXCEPTION(CalculationCancelled, progress.throwIfCancelled());

    QCOMPARE(describe(GraphNode::prepared("expA", 7)), QStringLiteral("prepared(expA#7)"));
    QVERIFY(GraphNode::prepared("expA", 7) != GraphNode::prepared("expA", 8));
    QVERIFY(GraphNode::prepared("expA", 7) != GraphNode::result("expA"));

    const PublishOutcome unset;
    QCOMPARE(unset.kind, PublishOutcome::Kind::RefusedGone);
    QCOMPARE(ComputedCalculation().kind, ComputedCalculation::Kind::Cancelled);
}

FLYSIGHT_TEST_MAIN(CalcEngineAsyncTest)
#include "tst_calcengine_async.moc"
