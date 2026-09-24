// CalculationEngine: blocker inspection - which explicit calculations stand
// between a public name and its availability - with synthetic calculations
// against a fake session state. Expected values are literals.
//
// Sensor-fusion-jobs acceptance 12 (inspection reports the explicit
// calculation behind a derived name, nothing after publication, and never runs
// it) and 13 (B consumes A: requesting blockers until none remain runs A then
// B), engine half.

#include <algorithm>
#include <optional>
#include <stdexcept>

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
using Synthetic::measKey;

namespace {

using Names = QSet<DependencyKey>;
using Prepare = CalculationEngine::PrepareOutcome;
using State = BlockerReport::State;
using Ready = CalculationReadiness::State;

// Declaration order matters: a registry must outlive its engines.
struct World {
    CalculationRegistry registry;
    FakePreferenceProvider prefs;
    FakeSessionState state;
    CalculationEngine engine;

    World()
        : engine(&state, &registry)
    {
        registry.setPreferenceProvider(&prefs);
        Synthetic::registerExplicitWorld(registry);
        state.setAttribute("EA_IN", 4);
        state.setAttribute("EB_IN", 10);
    }
};

QStringList ids(const QList<CalculationBlocker> &blockers)
{
    QStringList result;
    for (const CalculationBlocker &b : blockers)
        result.append(b.instanceId);
    return result;
}

QStringList ids(const QList<UnproducedNote> &notes)
{
    QStringList result;
    for (const UnproducedNote &n : notes)
        result.append(n.calculation.instanceId);
    return result;
}

PublishOutcome runAsync(CalculationEngine &engine, const CalculationBlocker &blocker, ComputeMode mode)
{
    Prepare prepared = engine.prepare(blocker.registrationId, blocker.instanceOutput);
    if (prepared.kind != Prepare::Kind::Ready || !prepared.ticket)
        return PublishOutcome();
    ComputedCalculation computed = computeOn(mode, *prepared.ticket);
    return prepared.ticket->publish(std::move(computed));
}

// One explicit calculation: input attribute `input`, output attribute `output`
// = input + 1; with `produce` false it sets nothing and gives `reason`.
CalculationDescriptor explicitPlusOne(const char *id, const char *input, const char *output,
                                      bool produce = true, const char *reason = "")
{
    const QString in = QString::fromLatin1(input);
    const QString out = QString::fromLatin1(output);
    const QString why = QString::fromLatin1(reason);
    CalculationDescriptor d;
    d.id = QString::fromLatin1(id);
    d.policy = EvaluationPolicy::Explicit;
    d.inputs = {CalcInput::attribute(in)};
    d.outputs = {DependencyKey::attribute(out)};
    d.compute = [in, out, produce, why](const EvaluationContext &ctx) {
        CalculationResult r;
        if (produce)
            r.setAttribute(out, ctx.attribute(in).toInt() + 1);
        else
            r.setReason(why);
        return r;
    };
    return d;
}

} // namespace

class CalcEngineBlockersTest : public QObject {
    Q_OBJECT

private slots:
    void derivedNameReportsExplicitBlocker();
    void missingInputIsNotABlocker();
    void chainedBlockers_data() { addComputeModeRows(); }
    void chainedBlockers();
    void ranAndDidNotProduce();
    void failedCalculationIsNotProduced();
    void availableThroughFallbackReportsNothing();
    void storedAndUnknownNames();
    void explicitFamilyInstance();
    void blockedWinsOverNotProduced();
    void ringsTerminate();
    void inspectionNeverRunsExplicit();
    void readinessStates();
};

// Sensor-fusion-jobs acceptance 12: the report sees through on-demand intermediates (the
// "accH is blocked by fusion" case), reports nothing after publication, and
// never triggers the explicit calculation.
void CalcEngineBlockersTest::derivedNameReportsExplicitBlocker()
{
    World w;

    // DDA is two on-demand levels above expA; EA_DIAG is its diagnostics output.
    for (const char *name : {"DDA", "DA", "EA1", "EA2", "EA_DIAG"}) {
        const BlockerReport report = w.engine.blockers(attr(name));
        QVERIFY2(report.state == State::Blocked, name);
        QCOMPARE(report.blockers.size(), 1);
        const CalculationBlocker &blocker = report.blockers.first();
        QCOMPARE(blocker.registrationId, QStringLiteral("expA"));
        QCOMPARE(blocker.instanceId, QStringLiteral("expA"));
        QCOMPARE(blocker.title, QStringLiteral("Explicit A"));
        QVERIFY(isEmptyName(blocker.instanceOutput));
        QVERIFY(report.notProduced.isEmpty());
    }
    QCOMPARE(w.engine.runCount("expA"), 0);
    QCOMPARE(w.engine.totalRunCount(), 0);
    QVERIFY(!w.engine.isAvailable(attr("DDA")));
    QCOMPARE(w.engine.resultStatus("expA"), std::optional<ResultStatus>(ResultStatus::NotRequested));

    // An outstanding ticket does not change the report ("pending" is the job
    // model's notion, not the engine's).
    Prepare prepared = w.engine.prepare("expA");
    QCOMPARE(prepared.kind, Prepare::Kind::Ready);
    QCOMPARE(w.engine.blockers(attr("DDA")).state, State::Blocked);
    QCOMPARE(ids(w.engine.blockers(attr("DDA")).blockers), QStringList({"expA"}));
    QCOMPARE(w.engine.readiness("expA").state, Ready::Ready);

    ComputedCalculation computed = computeOn(ComputeMode::StdThread, *prepared.ticket);
    QCOMPARE(prepared.ticket->publish(std::move(computed)).kind, PublishOutcome::Kind::Published);

    for (const char *name : {"DDA", "DA", "EA1", "EA2", "EA_DIAG"}) {
        const BlockerReport report = w.engine.blockers(attr(name));
        QVERIFY2(report.state == State::Available, name);
        QVERIFY(report.blockers.isEmpty());
        QVERIFY(report.notProduced.isEmpty());
    }
    QCOMPARE(w.engine.attribute("DDA"), QVariant(1105));
    QCOMPARE(w.engine.runCount("expA"), 1);
    QVERIFY(w.engine.verifyAgainstFresh(Synthetic::explicitNames()).isEmpty());
}

// Spec 7.2: availability of inputs is decided before policy. A calculation
// with nothing to compute reports missing input, never "not requested".
void CalcEngineBlockersTest::missingInputIsNotABlocker()
{
    {
        World w;
        w.state.removeAttribute("EA_IN");       // "a track with no IMU data"

        for (const DependencyKey &name : Synthetic::explicitNames()) {
            const BlockerReport report = w.engine.blockers(name);
            QVERIFY2(report.state == State::NotApplicable, qPrintable(describe(name)));
            QVERIFY(report.blockers.isEmpty());
            QVERIFY(report.notProduced.isEmpty());
        }
        const CalculationReadiness readiness = w.engine.readiness("expA");
        QCOMPARE(readiness.state, Ready::MissingInput);
        QVERIFY(readiness.blockers.isEmpty());
        QCOMPARE(w.engine.readiness("expB").state, Ready::MissingInput);

        const Prepare prepared = w.engine.prepare("expA");
        QCOMPARE(prepared.kind, Prepare::Kind::NothingToRun);
        QCOMPARE(prepared.status, ResultStatus::MissingInput);
        QVERIFY(prepared.blockers.isEmpty());
        const Prepare preparedB = w.engine.prepare("expB");
        QCOMPARE(preparedB.kind, Prepare::Kind::NothingToRun);  // never Blocked
        QCOMPARE(w.engine.totalRunCount(), 0);

        // Requested and cached as missing input: still not a blocker, still not "failed".
        QCOMPARE(w.engine.blockers(attr("DDA")).state, State::NotApplicable);
        QCOMPARE(w.engine.readiness("expA").state, Ready::MissingInput);

        // The input arrives: now it is one.
        w.state.setAttribute(w.engine, "EA_IN", 4);
        QCOMPARE(w.engine.blockers(attr("DDA")).state, State::Blocked);
        QCOMPARE(ids(w.engine.blockers(attr("DB")).blockers), QStringList({"expA"}));
    }
    {
        // One input blocked, another genuinely missing: requesting the blocker
        // could never help, so none is reported. ALL inputs are examined.
        World w;
        w.state.removeAttribute("EB_IN");
        QCOMPARE(w.engine.blockers(attr("EA2")).state, State::Blocked);
        for (const char *name : {"EB1", "DB"}) {
            const BlockerReport report = w.engine.blockers(attr(name));
            QVERIFY2(report.state == State::NotApplicable, name);
            QVERIFY(report.blockers.isEmpty());
        }
        QCOMPARE(w.engine.readiness("expB").state, Ready::MissingInput);
        QVERIFY(w.engine.readiness("expB").blockers.isEmpty());
        QCOMPARE(w.engine.prepare("expB").kind, Prepare::Kind::NothingToRun);
    }
}

// Sensor-fusion-jobs acceptance 13: explicit B consumes unrequested explicit A. The blocker is
// A; once A publishes, the blocker is B; a consumer keeps requesting blockers
// until none remain.
void CalcEngineBlockersTest::chainedBlockers()
{
    QFETCH(int, mode);
    World w;

    for (const char *name : {"EB1", "DB"}) {
        const BlockerReport report = w.engine.blockers(attr(name));
        QVERIFY2(report.state == State::Blocked, name);
        QCOMPARE(ids(report.blockers), QStringList({"expA"}));      // A alone, not B
    }

    // "Inspect, request each blocker, repeat", as the plot request logic will.
    QStringList ran;
    int rounds = 0;
    for (;; ++rounds) {
        QVERIFY2(rounds < 5, "the loop must terminate");
        const BlockerReport report = w.engine.blockers(attr("DB"));
        if (report.state != State::Blocked)
            break;
        for (const CalculationBlocker &blocker : report.blockers) {
            const PublishOutcome outcome = runAsync(w.engine, blocker, ComputeMode(mode));
            QCOMPARE(outcome.kind, PublishOutcome::Kind::Published);
            ran.append(blocker.instanceId);
        }
        if (rounds == 0) {
            // After A has published the blocker is B, for B's output and above.
            const BlockerReport next = w.engine.blockers(attr("EB1"));
            QCOMPARE(next.state, State::Blocked);
            QCOMPARE(ids(next.blockers), QStringList({"expB"}));
            QCOMPARE(next.blockers.first().title, QStringLiteral("Explicit B"));
            QCOMPARE(ids(w.engine.blockers(attr("DB")).blockers), QStringList({"expB"}));
        }
    }
    QCOMPARE(rounds, 2);
    QCOMPARE(ran, QStringList({"expA", "expB"}));
    QCOMPARE(w.engine.blockers(attr("DB")).state, State::Available);
    QCOMPARE(w.engine.blockers(attr("EB1")).state, State::Available);
    QCOMPARE(w.engine.attribute("DB"), QVariant(19));
    QCOMPARE(w.engine.runCount("expA"), 1);
    QCOMPARE(w.engine.runCount("expB"), 1);
    QVERIFY(w.engine.verifyAgainstFresh(Synthetic::explicitNames()).isEmpty());
}

// Spec 7.2, fourth bullet: "ran and did not produce" is distinguished from
// "never run". A rejection is a result (spec 6).
void CalcEngineBlockersTest::ranAndDidNotProduce()
{
    World w;
    w.state.setAttribute("EA_IN", -1);

    const CalculationBlocker expA = w.engine.blockers(attr("EA1")).blockers.value(0);
    const PublishOutcome outcome = runAsync(w.engine, expA, ComputeMode::QtThread);
    QCOMPARE(outcome.kind, PublishOutcome::Kind::Published);
    QCOMPARE(outcome.status, ResultStatus::Ok);             // it ran; the rejection is its result
    QCOMPARE(outcome.detail, QStringLiteral("negative input"));
    QCOMPARE(w.engine.resultDetail("expA"), QStringLiteral("negative input"));

    // Its outputs and everything derived from them, through expB.
    for (const char *name : {"EA1", "EA2", "DA", "DDA", "EB1", "DB"}) {
        const BlockerReport report = w.engine.blockers(attr(name));
        QVERIFY2(report.state == State::NotProduced, name);
        QVERIFY(report.blockers.isEmpty());
        QCOMPARE(report.notProduced.size(), 1);
        const UnproducedNote &note = report.notProduced.first();
        QCOMPARE(note.calculation.registrationId, QStringLiteral("expA"));
        QCOMPARE(note.calculation.title, QStringLiteral("Explicit A"));
        QCOMPARE(note.status, ResultStatus::Ok);
        QCOMPARE(note.detail, QStringLiteral("negative input"));
    }
    // The diagnostics output was produced.
    QCOMPARE(w.engine.blockers(attr("EA_DIAG")).state, State::Available);
    QCOMPARE(w.engine.attribute("EA_DIAG"), QVariant(QStringLiteral("rejected")));

    // The same inputs give the same answer, so nothing offers to run it again.
    QCOMPARE(w.engine.readiness("expA").state, Ready::Done);
    QCOMPARE(w.engine.readiness("expA").status, std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(w.engine.readiness("expB").state, Ready::MissingInput);
    QCOMPARE(w.engine.prepare("expA").kind, Prepare::Kind::AlreadyValid);
    QCOMPARE(w.engine.prepare("expB").kind, Prepare::Kind::NothingToRun);
    QCOMPARE(w.engine.runCount("expA"), 1);

    // A declared input changes: never run again, so blocked again.
    w.state.setAttribute(w.engine, "EA_IN", 4);
    QCOMPARE(w.engine.resultDetail("expA"), QString());
    for (const char *name : {"EA1", "DDA", "DB", "EA_DIAG"}) {
        const BlockerReport report = w.engine.blockers(attr(name));
        QVERIFY2(report.state == State::Blocked, name);
        QCOMPARE(ids(report.blockers), QStringList({"expA"}));
        QVERIFY(report.notProduced.isEmpty());
    }
    QVERIFY(w.engine.verifyAgainstFresh(Synthetic::explicitNames()).isEmpty());
}

void CalcEngineBlockersTest::failedCalculationIsNotProduced()
{
    World w;
    CalculationDescriptor thrower;
    thrower.id = QStringLiteral("thrower");
    thrower.title = QStringLiteral("Thrower");
    thrower.policy = EvaluationPolicy::Explicit;
    thrower.inputs = {CalcInput::attribute("EA_IN")};
    thrower.outputs = {attr("T1")};
    thrower.compute = [](const EvaluationContext &) -> CalculationResult {
        throw std::runtime_error("synthetic failure");
    };
    CalculationDescriptor above;
    above.id = QStringLiteral("aboveT");
    above.inputs = {CalcInput::attribute("T1")};
    above.outputs = {attr("T_ABOVE")};
    above.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute("T_ABOVE", ctx.attribute("T1"));
    };
    QVERIFY(w.registry.registerCalculation(thrower));
    QVERIFY(w.registry.registerCalculation(above));

    QCOMPARE(ids(w.engine.blockers(attr("T_ABOVE")).blockers), QStringList({"thrower"}));
    {
        WarningCapture warnings;
        QCOMPARE(w.engine.request("thrower").status, ResultStatus::Failed);
        QCOMPARE(warnings.count("synthetic failure"), 1);
    }

    for (const char *name : {"T1", "T_ABOVE"}) {
        const BlockerReport report = w.engine.blockers(attr(name));
        QVERIFY2(report.state == State::NotProduced, name);
        QVERIFY(report.blockers.isEmpty());
        QCOMPARE(report.notProduced.size(), 1);
        QCOMPARE(report.notProduced.first().calculation.instanceId, QStringLiteral("thrower"));
        QCOMPARE(report.notProduced.first().calculation.title, QStringLiteral("Thrower"));
        QCOMPARE(report.notProduced.first().status, ResultStatus::Failed);
        QCOMPARE(report.notProduced.first().detail, QStringLiteral("synthetic failure"));
    }
    QCOMPARE(w.engine.readiness("thrower").state, Ready::Done);
    QCOMPARE(w.engine.readiness("thrower").status, std::optional<ResultStatus>(ResultStatus::Failed));
    QCOMPARE(w.engine.runCount("thrower"), 1);
}

void CalcEngineBlockersTest::availableThroughFallbackReportsNothing()
{
    World w;
    // FB: an explicit candidate first, then an on-demand constant.
    CalculationDescriptor constFB;
    constFB.id = QStringLiteral("constFB");
    constFB.outputs = {attr("FB")};
    constFB.compute = [](const EvaluationContext &) { return CalculationResult().setAttribute("FB", 42); };
    QVERIFY(w.registry.registerCalculation(explicitPlusOne("expFB", "EA_IN", "FB")));
    QVERIFY(w.registry.registerCalculation(constFB));

    const BlockerReport report = w.engine.blockers(attr("FB"));
    QCOMPARE(report.state, State::Available);
    QVERIFY(report.blockers.isEmpty());         // nothing stands between FB and a value
    QVERIFY(report.notProduced.isEmpty());
    QCOMPARE(w.engine.attribute("FB"), QVariant(42));
    QCOMPARE(w.engine.runCount("expFB"), 0);

    // The calculation itself could still be requested, and then it wins.
    QCOMPARE(w.engine.readiness("expFB").state, Ready::Ready);
    QCOMPARE(w.engine.request("expFB").status, ResultStatus::Ok);
    QCOMPARE(w.engine.attribute("FB"), QVariant(5));
    QCOMPARE(w.engine.blockers(attr("FB")).state, State::Available);
}

void CalcEngineBlockersTest::storedAndUnknownNames()
{
    World w;

    // A stored attribute wins; the candidates behind the name are not consulted.
    w.state.setAttribute(w.engine, "EA1", 99);
    QCOMPARE(w.engine.blockers(attr("EA1")).state, State::Available);
    QCOMPARE(w.engine.blockers(attr("DA")).state, State::Available);
    QCOMPARE(w.engine.attribute("DA"), QVariant(199));

    // Even when its value is invalid: then the name is simply unavailable.
    w.state.setAttribute(w.engine, "EA1", QVariant());
    const BlockerReport stored = w.engine.blockers(attr("EA1"));
    QCOMPARE(stored.state, State::NotApplicable);
    QVERIFY(stored.blockers.isEmpty());
    QCOMPARE(w.engine.blockers(attr("DDA")).state, State::NotApplicable);
    // EA2 is not overridden.
    QCOMPARE(ids(w.engine.blockers(attr("EA2")).blockers), QStringList({"expA"}));

    // Unknown names, and measurements.
    QCOMPARE(w.engine.blockers(attr("no such name")).state, State::NotApplicable);
    QCOMPARE(w.engine.blockers(measKey("S", "nothing")).state, State::NotApplicable);
    w.state.setMeasurement(w.engine, "S", "m", {1.0, 2.0});
    QCOMPARE(w.engine.blockers(measKey("S", "m")).state, State::Available);
    w.state.setMeasurement(w.engine, "S", "empty", {});
    QCOMPARE(w.engine.blockers(measKey("S", "empty")).state, State::NotApplicable);
    QCOMPARE(w.engine.totalRunCount(), 1);      // derivA, for the stored EA1
}

// A blocker carries what prepare() / request() need to find a family instance.
void CalcEngineBlockersTest::explicitFamilyInstance()
{
    World w;
    CalculationFamily family;
    family.id = QStringLiteral("xneg");
    family.policy = EvaluationPolicy::Explicit;
    family.instantiate = [](const DependencyKey &name) -> std::optional<CalculationDescriptor> {
        const QString prefix = QStringLiteral("xneg:");
        if (name.type != DependencyKey::Type::Attribute || !name.attributeKey.startsWith(prefix))
            return std::nullopt;
        const QString output = name.attributeKey;
        const QString source = output.mid(prefix.size());
        if (source.isEmpty())
            return std::nullopt;
        CalculationDescriptor d;
        d.id = output;      // instance key
        d.title = QStringLiteral("Negate %1").arg(source);
        d.inputs = {CalcInput::attribute(source)};
        d.outputs = {DependencyKey::attribute(output)};
        d.compute = [output, source](const EvaluationContext &ctx) {
            return CalculationResult().setAttribute(output, -ctx.attribute(source).toInt());
        };
        return d;
    };
    QVERIFY(w.registry.registerFamily(family));

    const DependencyKey name = attr("xneg:EA_IN");
    const BlockerReport report = w.engine.blockers(name);
    QCOMPARE(report.state, State::Blocked);
    QCOMPARE(report.blockers.size(), 1);
    const CalculationBlocker blocker = report.blockers.first();
    QCOMPARE(blocker.registrationId, QStringLiteral("xneg"));
    QCOMPARE(blocker.instanceId, QStringLiteral("xneg#xneg:EA_IN"));
    QVERIFY(blocker.instanceOutput == name);
    QCOMPARE(blocker.title, QStringLiteral("Negate EA_IN"));
    QCOMPARE(w.engine.readiness("xneg", name).state, Ready::Ready);
    QCOMPARE(w.engine.readiness("xneg").state, Ready::Unknown);     // a family needs a name
    // An instance over a missing input is not a blocker.
    QCOMPARE(w.engine.blockers(attr("xneg:absent")).state, State::NotApplicable);

    // The pair round-trips into prepare().
    Prepare prepared = w.engine.prepare(blocker.registrationId, blocker.instanceOutput);
    QCOMPARE(prepared.kind, Prepare::Kind::Ready);
    QCOMPARE(prepared.ticket->registrationId(), QStringLiteral("xneg"));
    QCOMPARE(prepared.ticket->instanceId(), QStringLiteral("xneg#xneg:EA_IN"));
    QCOMPARE(prepared.ticket->title(), QStringLiteral("Negate EA_IN"));
    ComputedCalculation computed = computeOn(ComputeMode::StdThread, *prepared.ticket);
    QCOMPARE(prepared.ticket->publish(std::move(computed)).kind, PublishOutcome::Kind::Published);

    QCOMPARE(w.engine.attribute("xneg:EA_IN"), QVariant(-4));
    QCOMPARE(w.engine.blockers(name).state, State::Available);
    QCOMPARE(w.engine.readiness("xneg", name).state, Ready::Done);
    QCOMPARE(w.engine.resultDetail("xneg", name), QString());
    QCOMPARE(w.engine.runCount("xneg"), 1);

    // Unregistering the family between prepare and publish refuses, too.
    Prepare other = w.engine.prepare("xneg", attr("xneg:EB_IN"));
    QCOMPARE(other.kind, Prepare::Kind::Ready);
    QVERIFY(w.registry.unregister("xneg", CalculationRegistry::Removal::Change));
    QCOMPARE(other.ticket->publish(other.ticket->compute()).reason,
             PublishOutcome::Reason::RegistrationRemoved);
}

// State precedence: something that can still be requested outranks something
// that ran in vain; both are reported.
void CalcEngineBlockersTest::blockedWinsOverNotProduced()
{
    World w;
    QVERIFY(w.registry.registerCalculation(explicitPlusOne("ranInVain", "EA_IN", "N2", false, "no luck")));
    QVERIFY(w.registry.registerCalculation(explicitPlusOne("fresh", "EB_IN", "N2")));

    BlockerReport report = w.engine.blockers(attr("N2"));
    QCOMPARE(report.state, State::Blocked);
    QCOMPARE(ids(report.blockers), QStringList({"ranInVain", "fresh"}));    // candidate order
    QVERIFY(report.notProduced.isEmpty());

    QCOMPARE(w.engine.request("ranInVain").status, ResultStatus::Ok);
    report = w.engine.blockers(attr("N2"));
    QCOMPARE(report.state, State::Blocked);
    QCOMPARE(ids(report.blockers), QStringList({"fresh"}));
    QCOMPARE(ids(report.notProduced), QStringList({"ranInVain"}));
    QCOMPARE(report.notProduced.first().detail, QStringLiteral("no luck"));
    QCOMPARE(report.notProduced.first().calculation.title, QStringLiteral("ranInVain"));   // no title: the id

    QCOMPARE(w.engine.request("fresh").status, ResultStatus::Ok);
    report = w.engine.blockers(attr("N2"));
    QCOMPARE(report.state, State::Available);
    QVERIFY(report.notProduced.isEmpty());
    QCOMPARE(w.engine.attribute("N2"), QVariant(11));
}

// A set of names being walked ends rings; inspection terminates on any graph.
void CalcEngineBlockersTest::ringsTerminate()
{
    CalculationRegistry registry;
    Synthetic::registerTangleWorld(registry);
    Synthetic::registerExplicitWorld(registry);

    // The ring through an explicit calculation's own output, and an explicit
    // calculation hanging off the tangle.
    CalculationDescriptor feedsE;
    feedsE.id = QStringLiteral("feedsE");
    feedsE.inputs = {CalcInput::attribute("EX")};
    feedsE.outputs = {attr("EY")};
    feedsE.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute("EY", ctx.attribute("EX").toInt() * 2);
    };
    QVERIFY(registry.registerCalculation(explicitPlusOne("explicitE", "EY", "EX")));
    QVERIFY(registry.registerCalculation(feedsE));
    QVERIFY(registry.registerCalculation(explicitPlusOne("offTangle", "K3", "OFF")));
    QVERIFY(registry.registerCalculation(explicitPlusOne("offRing", "OY", "OFF2")));

    FakeSessionState state;
    CalculationEngine engine(&state, &registry);
    WarningCapture warnings;    // the rings are reported on every detection

    QList<DependencyKey> names = Synthetic::tangleNames();
    names << attr("EX") << attr("EY") << attr("OFF") << attr("OFF2");
    for (int pass = 0; pass < 2; ++pass) {
        for (const DependencyKey &name : std::as_const(names)) {
            const BlockerReport report = engine.blockers(name);
            const bool available = engine.isAvailable(name);
            QVERIFY2((report.state == State::Available) == available, qPrintable(describe(name)));
        }
        std::reverse(names.begin(), names.end());
    }
    for (const QString &id : registry.registeredIds())
        engine.readiness(id);

    QCOMPARE(engine.blockers(attr("OX")).state, State::Available);
    QCOMPARE(engine.blockers(attr("OY")).state, State::NotApplicable);
    QCOMPARE(engine.blockers(attr("EX")).state, State::NotApplicable);      // a ring is not a blocker
    QCOMPARE(engine.readiness("explicitE").state, Ready::MissingInput);
    QCOMPARE(ids(engine.blockers(attr("OFF")).blockers), QStringList({"offTangle"}));
    QCOMPARE(engine.blockers(attr("OFF2")).state, State::NotApplicable);
    QCOMPARE(engine.scopeDepth(), 0);
    QCOMPARE(engine.runCount("explicitE") + engine.runCount("offTangle") + engine.runCount("offRing"), 0);
    QVERIFY(engine.verifyAgainstFresh(names).isEmpty());
}

// Spec 7.2, last bullet: inspection may compute cheap on-demand inputs; it
// never computes an explicit calculation and never changes what a later read
// returns.
void CalcEngineBlockersTest::inspectionNeverRunsExplicit()
{
    World w;
    Synthetic::registerSharedWorld(w.registry);
    QVERIFY(w.registry.registerCalculation(explicitPlusOne("overX", "X", "OVER_X")));
    w.state.setAttribute("A", 1);
    w.state.setAttribute("B", 2);
    w.prefs.set("p", 5);

    QList<DependencyKey> names = Synthetic::explicitNames();
    names << attr("X") << attr("Y") << attr("OVER_X") << attr("neg:EA1") << attr("unknown");
    const QStringList explicitIds = {"expA", "expB", "overX"};
    const auto explicitRuns = [&w, &explicitIds]() {
        int runs = 0;
        for (const QString &id : explicitIds)
            runs += w.engine.runCount(id);
        return runs;
    };

    QVERIFY(w.engine.verifyAgainstFresh(names).isEmpty());
    for (int pass = 0; pass < 3; ++pass) {
        for (const DependencyKey &name : std::as_const(names))
            w.engine.blockers(name);
        for (const QString &id : w.registry.registeredIds())
            w.engine.readiness(id);
        w.engine.readiness("neg", attr("neg:EA1"));
        std::reverse(names.begin(), names.end());
        QCOMPARE(explicitRuns(), 0);
        QVERIFY(w.engine.verifyAgainstFresh(names).isEmpty());
    }
    // It did compute the on-demand input of overX, once.
    QCOMPARE(ids(w.engine.blockers(attr("OVER_X")).blockers), QStringList({"overX"}));
    QCOMPARE(w.engine.runCount("sum"), 1);
    QCOMPARE(ids(w.engine.blockers(attr("neg:EA1")).blockers), QStringList({"expA"}));

    // The same holds once things have been published and then invalidated.
    QCOMPARE(w.engine.request("expA").status, ResultStatus::Ok);
    QCOMPARE(w.engine.request("overX").status, ResultStatus::Ok);
    const int runs = explicitRuns();
    QCOMPARE(runs, 2);
    for (const DependencyKey &name : std::as_const(names))
        w.engine.blockers(name);
    QCOMPARE(w.engine.blockers(attr("neg:EA1")).state, State::Available);
    w.state.setAttribute(w.engine, "EA_IN", 5);
    w.state.setAttribute(w.engine, "A", 3);
    for (const DependencyKey &name : std::as_const(names))
        w.engine.blockers(name);
    QCOMPARE(explicitRuns(), runs);
    QCOMPARE(w.engine.scopeDepth(), 0);
    QVERIFY(w.engine.verifyAgainstFresh(names).isEmpty());
}

void CalcEngineBlockersTest::readinessStates()
{
    World w;
    Synthetic::registerSharedWorld(w.registry);
    w.state.setAttribute("A", 1);
    w.state.setAttribute("B", 2);

    // Unknown
    QCOMPARE(w.engine.readiness("no such id").state, Ready::Unknown);
    QCOMPARE(w.engine.readiness("neg").state, Ready::Unknown);
    QCOMPARE(w.engine.readiness("neg", attr("notOfThisFamily")).state, Ready::Unknown);

    // Ready: explicit, inputs available, never run. "Can a job be created?"
    CalculationReadiness r = w.engine.readiness("expA");
    QCOMPARE(r.state, Ready::Ready);
    QVERIFY(r.blockers.isEmpty());
    QVERIFY(!r.status);

    // Blocked: explicit or on demand, waiting for an explicit calculation.
    r = w.engine.readiness("expB");
    QCOMPARE(r.state, Ready::Blocked);
    QCOMPARE(ids(r.blockers), QStringList({"expA"}));
    r = w.engine.readiness("derivA2");
    QCOMPARE(r.state, Ready::Blocked);
    QCOMPARE(ids(r.blockers), QStringList({"expA"}));
    QCOMPARE(w.engine.readiness("neg", attr("neg:DB")).state, Ready::Blocked);

    // Done: on demand with its inputs available - whether or not it has run.
    r = w.engine.readiness("sum");
    QCOMPARE(r.state, Ready::Done);
    QVERIFY(!r.status);
    QCOMPARE(w.engine.runCount("sum"), 0);
    QCOMPARE(w.engine.attribute("X"), QVariant(3));
    QCOMPARE(w.engine.readiness("sum").status, std::optional<ResultStatus>(ResultStatus::Ok));

    // MissingInput: a missing preference, a missing attribute.
    QCOMPARE(w.engine.readiness("triple").state, Ready::MissingInput);     // p is not set
    QCOMPARE(w.engine.readiness("fallbackX").state, Ready::MissingInput);  // C is not stored
    w.prefs.set(w.registry, "p", 5);
    QCOMPARE(w.engine.readiness("triple").state, Ready::Done);

    // Done: explicit with a valid result.
    QCOMPARE(w.engine.request("expA").status, ResultStatus::Ok);
    r = w.engine.readiness("expA");
    QCOMPARE(r.state, Ready::Done);
    QCOMPARE(r.status, std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(w.engine.readiness("expB").state, Ready::Ready);
    QCOMPARE(w.engine.readiness("derivA2").state, Ready::Done);

    // Back to Ready when a declared input changes.
    w.state.setAttribute(w.engine, "EA_IN", 5);
    QCOMPARE(w.engine.readiness("expA").state, Ready::Ready);
    QCOMPARE(w.engine.readiness("expB").state, Ready::Blocked);
    QCOMPARE(w.engine.runCount("expA"), 1);
}

FLYSIGHT_TEST_MAIN(CalcEngineBlockersTest)
#include "tst_calcengine_blockers.moc"
