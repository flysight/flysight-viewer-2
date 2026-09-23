// CalculationEngine: stored results. Export of an installed explicit result as
// a StoredCalculationResult (bundle, detail, result version, the leaves it
// reached and their input fingerprint), restore of such a snapshot as though it
// had just been published, and the explicit-result listener, with synthetic
// calculations against a fake session state.
//
// Expected values are literals, except where the engine that published IS the
// expectation: "a restored result cannot be told from a published one" is the
// rule under test.

#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include <QSet>
#include <QtTest>

#include "asyncdriver.h"
#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "engine/storedcalculationresult.h"
#include "fakesessionstate.h"
#include "testmain.h"
#include "testutil.h"

using namespace FlySight;
using namespace FlySightTest;
using Synthetic::attr;

namespace {

using Names = QSet<DependencyKey>;
using Prepare = CalculationEngine::PrepareOutcome;
using Restore = CalculationEngine::RestoreOutcome;
using StaleCheck = Restore::StaleCheck;
using Event = CalculationEngine::ExplicitResultEvent;

// ---- local calculations ----------------------------------------------------

// withPref: Explicit; inputs attr EA_IN, pref p; WP = EA_IN * 100 + p
// (EA_IN = 4, p = 5: WP = 405).
CalculationDescriptor withPref()
{
    CalculationDescriptor d;
    d.id = QStringLiteral("withPref");
    d.policy = EvaluationPolicy::Explicit;
    d.inputs = {CalcInput::attribute("EA_IN"), CalcInput::preference("p")};
    d.outputs = {attr("WP")};
    d.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute("WP", ctx.attribute("EA_IN").toInt() * 100
                                                          + ctx.preference("p").toInt());
    };
    return d;
}

// measExplicit: Explicit; input meas S/m; MS = the number of samples of S/m
// (an int, so NaN inputs still give a comparable output).
CalculationDescriptor measExplicit()
{
    CalculationDescriptor d;
    d.id = QStringLiteral("measExplicit");
    d.policy = EvaluationPolicy::Explicit;
    d.inputs = {CalcInput::measurement("S", "m")};
    d.outputs = {attr("MS")};
    d.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute("MS", int(ctx.measurement("S", "m").size()));
    };
    return d;
}

// versioned: expA's shape (EA1 = EA_IN + 1, EA2 = EA_IN * 2, EA_DIAG = "ok")
// under id verA, declaring `version` as its result version.
CalculationDescriptor versioned(const QString &version)
{
    CalculationDescriptor d = Synthetic::expA();
    d.id = QStringLiteral("verA");
    d.title = QStringLiteral("Versioned A");
    d.resultVersion = version;
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

// altIn: on demand; no inputs; EA_IN = 0. Never wins over the stored EA_IN;
// registered and unregistered only to cause registry changes.
CalculationDescriptor altIn()
{
    CalculationDescriptor d;
    d.id = QStringLiteral("altIn");
    d.outputs = {attr("EA_IN")};
    d.compute = [](const EvaluationContext &) { return CalculationResult().setAttribute("EA_IN", 0); };
    return d;
}

using Registrar = std::function<void(CalculationRegistry &)>;

/// The shared world, the explicit world (expA, derivA, derivA2, expB, derivB)
/// and withPref, measExplicit, thrower.
void registerStandard(CalculationRegistry &registry)
{
    Synthetic::registerSharedWorld(registry);
    Synthetic::registerExplicitWorld(registry);
    registry.registerCalculation(withPref());
    registry.registerCalculation(measExplicit());
    registry.registerCalculation(thrower());
}

// ---- worlds ------------------------------------------------------------------

/// A private registry and its preferences (p = 5). Declared before the
/// sessions that use it: a registry must outlive its engines.
struct Registry {
    CalculationRegistry registry;
    FakePreferenceProvider prefs;

    explicit Registry(const Registrar &registrar = registerStandard)
    {
        registry.setPreferenceProvider(&prefs);
        registrar(registry);
        prefs.set("p", 5);
    }
};

/// One session: state (EA_IN = 4, EB_IN = 10) and its engine, recording what
/// both listeners receive.
struct Session {
    FakeSessionState state;
    CalculationEngine engine;
    QList<Names> broadcasts;        // the invalidation listener
    QList<Event> events;            // the explicit-result listener
    QStringList log;                // both, in call order

    explicit Session(Registry &r)
        : engine(&state, &r.registry)
    {
        engine.setInvalidationListener([this](const Names &names) {
            broadcasts.append(names);
            log.append(QStringLiteral("names"));
        });
        engine.setExplicitResultListener([this](const Event &event) {
            events.append(event);
            log.append(QStringLiteral("explicit ") + event.instanceId);
        });
        state.setAttribute("EA_IN", 4);
        state.setAttribute("EB_IN", 10);
    }

    QList<Event> takeEvents() { return std::exchange(events, {}); }
};

/// Two sessions on one registry: one that publishes, one that restores.
struct Pair {
    Registry reg;
    Session source;
    Session target;

    explicit Pair(const Registrar &registrar = registerStandard)
        : reg(registrar), source(reg), target(reg)
    {
    }
};

// ---- text forms, for comparisons with readable failures --------------------

QString statusText(ResultStatus status)
{
    switch (status) {
    case ResultStatus::Ok:             return QStringLiteral("Ok");
    case ResultStatus::MissingInput:   return QStringLiteral("MissingInput");
    case ResultStatus::NotRequested:   return QStringLiteral("NotRequested");
    case ResultStatus::Cycle:          return QStringLiteral("Cycle");
    case ResultStatus::Failed:         return QStringLiteral("Failed");
    case ResultStatus::UndeclaredRead: return QStringLiteral("UndeclaredRead");
    case ResultStatus::InvalidOutput:  return QStringLiteral("InvalidOutput");
    }
    return QString();
}

/// "Installed expA Ok", "DroppedByInputChange expB MissingInput"
QStringList describeEvents(const QList<Event> &events)
{
    QStringList text;
    for (const Event &event : events) {
        const QString kind = event.kind == Event::Kind::Installed ? QStringLiteral("Installed")
                                                                  : QStringLiteral("DroppedByInputChange");
        text.append(kind + QLatin1Char(' ') + event.instanceId + QLatin1Char(' ') + statusText(event.status));
    }
    return text;
}

QStringList describeLeaves(const QList<GraphNode> &leaves)
{
    QStringList text;
    for (const GraphNode &leaf : leaves)
        text.append(describe(leaf));
    return text;
}

double fromBits(quint64 bits)
{
    double v = 0.0;
    std::memcpy(&v, &bits, sizeof v);
    return v;
}

bool isNotRun(const std::optional<ResultStatus> &status)
{
    return !status || *status == ResultStatus::NotRequested;
}

/// Everything restore must reproduce about a blocker report.
QString reportText(const BlockerReport &report)
{
    QStringList text;
    text.append(QString::number(int(report.state)));
    for (const CalculationBlocker &blocker : report.blockers)
        text.append(QStringLiteral("blocker ") + blocker.instanceId);
    for (const UnproducedNote &note : report.notProduced)
        text.append(QStringLiteral("note %1 %2 %3").arg(note.calculation.instanceId, statusText(note.status), note.detail));
    return text.join(QStringLiteral("; "));
}

/// The engine's counters that an inspection must leave alone.
struct EngineCounters {
    int cachedNodes, edges, runs, prepared;

    explicit EngineCounters(const CalculationEngine &e)
        : cachedNodes(e.cachedNodeCount()), edges(e.edgeCount()), runs(e.totalRunCount()),
          prepared(e.preparedCount())
    {
    }
    bool operator==(const EngineCounters &o) const
    {
        return cachedNodes == o.cachedNodes && edges == o.edges && runs == o.runs && prepared == o.prepared;
    }
};

} // namespace

class CalcEngineRestoreTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void fingerprintKnownAnswer();
    void fingerprintCanonicalForms();
    void leafKindCodes();
    void exportOnlyInstalledOk();
    void exportLeaves();
    void restoreIntoFreshEngine();
    void restoreRejection();
    void restoreNeverReplaces();
    void restoreStaleChecks_data();
    void restoreStaleChecks();
    void restoreChain();
    void restoreIgnoresAttributeType();
    void restoreNotFound();
    void restoreBeatsOutstandingTicket_data() { addComputeModeRows(); }
    void restoreBeatsOutstandingTicket();
    void restoredResultInvalidatesLikePublished();
    void restoreNaNPayloadAndSignedZero();
    void sameContentBitExactAttributes();
    void installedOnSyncAndAsync_data() { addComputeModeRows(); }
    void installedOnSyncAndAsync();
    void installedForEveryStatus();
    void restoreIsNotAnInstall();
    void droppedByInputChange();
    void droppedByPreferenceAndSource();
    void droppedByRequestOfUpstream();
    void noDropEventWithoutInputChange();

private:
    QList<CalculationId> m_globalBefore;
};

// Private registries only: the global registry is untouched and no engine is
// left enrolled anywhere.
void CalcEngineRestoreTest::init()
{
    m_globalBefore = CalculationRegistry::instance().registeredIds();
}

void CalcEngineRestoreTest::cleanup()
{
    QCOMPARE(CalculationRegistry::instance().registeredIds(), m_globalBefore);
    QCOMPARE(CalculationRegistry::instance().enrolledEngineCount(), 0);
}

// The documented encoding, byte by byte, and its SHA-256 computed outside the
// code under test.
void CalcEngineRestoreTest::fingerprintKnownAnswer()
{
    FakeSessionState state;
    state.setAttribute("K", QVariant(1.5));
    state.setMeasurement("S", "m", {1.0, -0.0, qQNaN()}, "u");
    FakePreferenceProvider prefs;
    prefs.set("p", 5);

    const QList<GraphNode> leaves = {
        GraphNode::storedAttribute("K"),
        GraphNode::storedAttribute("Z"),            // absent
        GraphNode::sourceMeasurement("S", "m"),
        GraphNode::sourceUnit("S", "m"),
        GraphNode::preference("p"),
        GraphNode::preference("q"),                 // absent
    };

    const QByteArray expected = QByteArray::fromHex(
        "666c7973696768742d696e707574732d7631" "00"     // magic "flysight-inputs-v1", 0x00
        "06000000"                                      // 6 leaves
        // storedAttribute K
        "00" "01000000" "4b" "00000000"                 //   kind 0, a "K", b ""
        "01" "01" "03000000" "312e35"                   //   present, text "1.5"
        // storedAttribute Z
        "00" "01000000" "5a" "00000000"                 //   kind 0, a "Z", b ""
        "00"                                            //   absent
        // sourceMeasurement S/m
        "01" "01000000" "53" "01000000" "6d"            //   kind 1, a "S", b "m"
        "01" "0300000000000000"                         //   present, 3 samples
        "000000000000f03f"                              //   1.0
        "0000000000000080"                              //   -0.0
        "000000000000f87f"                              //   NaN (canonical)
        // sourceUnit S/m
        "02" "01000000" "53" "01000000" "6d"            //   kind 2, a "S", b "m"
        "01" "01000000" "75"                            //   present, "u"
        // preference p
        "03" "01000000" "70" "00000000"                 //   kind 3, a "p", b ""
        "01" "01" "01000000" "35"                       //   present, text "5"
        // preference q
        "03" "01000000" "71" "00000000"                 //   kind 3, a "q", b ""
        "00");                                          //   absent

    QCOMPARE(inputFingerprintEncoding(leaves, state, &prefs).toHex(), expected.toHex());

    // python -c "import hashlib; print(hashlib.sha256(bytes.fromhex('666c7973696768742d696e707574732d7631000600000000010000004b00000000010103000000312e3500010000005a0000000000010100000053010000006d010300000000000000000000000000f03f0000000000000080000000000000f87f020100000053010000006d01010000007503010000007000000000010101000000350301000000710000000000')).hexdigest())"
    const QByteArray digest = inputFingerprint(leaves, state, &prefs);
    QCOMPARE(digest.size(), InputFingerprintSize);
    QCOMPARE(digest.toHex(), QByteArray("056f9d9b84ca6bde330fbcf0b70340f1b46182d252121efa575a00a055180b32"));
}

void CalcEngineRestoreTest::fingerprintCanonicalForms()
{
    const QList<GraphNode> meas = {GraphNode::sourceMeasurement("S", "m")};
    const QList<GraphNode> attrK = {GraphNode::storedAttribute("K")};
    const QList<GraphNode> prefP = {GraphNode::preference("p")};
    const auto samples = [&meas](const QVector<double> &v) {
        FakeSessionState state;
        state.setMeasurement("S", "m", v, "u");
        return inputFingerprint(meas, state, nullptr);
    };
    const auto attribute = [&attrK](const std::optional<QVariant> &v) {
        FakeSessionState state;
        if (v)
            state.setAttribute("K", *v);
        return inputFingerprint(attrK, state, nullptr);
    };
    const auto preference = [&prefP](const QVariant &v) {
        FakeSessionState state;
        FakePreferenceProvider prefs;
        prefs.set("p", v);
        return inputFingerprint(prefP, state, &prefs);
    };

    // Every NaN is one NaN, whatever its sign and payload
    QCOMPARE(samples({1.0, qQNaN()}), samples({1.0, fromBits(Q_UINT64_C(0xFFF8000000000123))}));
    QCOMPARE(samples({1.0, qQNaN()}), samples({1.0, fromBits(Q_UINT64_C(0x7FF0000000000001))}));
    // ...but signed zero and the infinities are distinct values
    QVERIFY(samples({-0.0}) != samples({0.0}));
    QVERIFY(samples({qInf()}) != samples({-qInf()}));
    QVERIFY(samples({qInf()}) != samples({qQNaN()}));
    // A present measurement without samples is not an absent one
    {
        FakeSessionState absent;
        QVERIFY(samples({}) != inputFingerprint(meas, absent, nullptr));
    }

    // Attributes are fingerprinted as the session file's text
    QCOMPARE(attribute(QVariant(1.5)), attribute(QVariant(QStringLiteral("1.5"))));
    QCOMPARE(attribute(QVariant(4)), attribute(QVariant(QStringLiteral("4"))));
    QCOMPARE(attribute(QVariant(qlonglong(4))), attribute(QVariant(4)));
    QVERIFY(attribute(QVariant(4)) != attribute(QVariant(5)));
    // Absent, present with "", and present with an invalid value are three things
    QVERIFY(attribute(std::nullopt) != attribute(QVariant(QString())));
    QVERIFY(attribute(std::nullopt) != attribute(QVariant()));
    QVERIFY(attribute(QVariant()) != attribute(QVariant(QString())));

    // Preferences: the same text form; an invalid value is an absent preference
    QCOMPARE(preference(QVariant(5)), preference(QVariant(QStringLiteral("5"))));
    QVERIFY(preference(QVariant(5)) != preference(QVariant(6)));
    {
        FakeSessionState state;
        QCOMPARE(preference(QVariant()), inputFingerprint(prefP, state, nullptr));
    }

    // Leaf identity is part of the encoding: the same value under another name differs
    {
        FakeSessionState state;
        state.setAttribute("K", 1);
        state.setAttribute("L", 1);
        QVERIFY(inputFingerprint(attrK, state, nullptr)
                != inputFingerprint({GraphNode::storedAttribute("L")}, state, nullptr));
    }
}

void CalcEngineRestoreTest::leafKindCodes()
{
    const QList<GraphNode> leaves = {GraphNode::storedAttribute("K"), GraphNode::sourceMeasurement("S", "m"),
                                     GraphNode::sourceUnit("S", "m"), GraphNode::preference("p")};
    for (int i = 0; i < leaves.size(); ++i) {
        const GraphNode &leaf = leaves.at(i);
        QVERIFY(isStoredLeafKind(leaf.kind));
        QCOMPARE(int(storedLeafKindCode(leaf.kind)), i);
        const std::optional<GraphNode> back = storedLeafFromCode(quint8(i), leaf.a, leaf.b);
        QVERIFY(back.has_value());
        QVERIFY(*back == leaf);
        QVERIFY(!back->measurementName);
    }
    QVERIFY(!storedLeafFromCode(4, "a", QString()).has_value());
    QVERIFY(!storedLeafFromCode(0xFF, "a", QString()).has_value());

    QVERIFY(!isStoredLeafKind(GraphNode::resolution(attr("K")).kind));
    QVERIFY(!isStoredLeafKind(GraphNode::result("expA").kind));
    QVERIFY(!isStoredLeafKind(GraphNode::prepared("expA", 1).kind));

    // The order: kind code, then a, then b; case-sensitive UTF-16 code units
    QVERIFY(storedLeafLess(GraphNode::preference("A"), GraphNode::storedAttribute("Z")) == false);
    QVERIFY(storedLeafLess(GraphNode::storedAttribute("Z"), GraphNode::sourceMeasurement("A", "a")));
    QVERIFY(storedLeafLess(GraphNode::sourceMeasurement("S", "m"), GraphNode::sourceUnit("A", "a")));
    QVERIFY(storedLeafLess(GraphNode::storedAttribute("EA2"), GraphNode::storedAttribute("EA_IN")));
    QVERIFY(storedLeafLess(GraphNode::storedAttribute("B"), GraphNode::storedAttribute("a")));
    QVERIFY(storedLeafLess(GraphNode::sourceMeasurement("S", "a"), GraphNode::sourceMeasurement("S", "b")));
    QVERIFY(!storedLeafLess(GraphNode::storedAttribute("K"), GraphNode::storedAttribute("K")));
}

void CalcEngineRestoreTest::exportOnlyInstalledOk()
{
    {
        Pair w;
        CalculationEngine &engine = w.source.engine;

        // Nothing cached, on demand, unknown, a family
        QVERIFY(!engine.exportResult("expA").has_value());
        QVERIFY(!engine.exportResult("nope").has_value());
        QVERIFY(!engine.exportResult("neg").has_value());
        QVERIFY(!engine.isAvailable(attr("EA1")));      // caches "not requested"
        QVERIFY(!engine.exportResult("expA").has_value());

        QCOMPARE(engine.request("expA").status, ResultStatus::Ok);
        QCOMPARE(engine.attribute("DA"), QVariant(105));
        QVERIFY(!engine.exportResult("derivA").has_value());
        QVERIFY(!engine.exportResult("neg").has_value());

        const EngineCounters before(engine);
        const std::optional<StoredCalculationResult> snapshot = engine.exportResult("expA");
        QVERIFY(snapshot.has_value());
        QVERIFY(EngineCounters(engine) == before);  // an inspection (it reads state, which is not counted here)

        QCOMPARE(snapshot->calculationId, QStringLiteral("expA"));
        QVERIFY(snapshot->resultVersion.isEmpty());
        QVERIFY(snapshot->detail.isEmpty());
        QVERIFY(snapshot->bundle.reason().isEmpty());
        QVERIFY(snapshot->bundle.setOutputs() == QList<DependencyKey>({attr("EA1"), attr("EA2"), attr("EA_DIAG")}));
        QCOMPARE(snapshot->bundle.attributeValue("EA1"), QVariant(5));
        QCOMPARE(snapshot->bundle.attributeValue("EA2"), QVariant(8));
        QCOMPARE(snapshot->bundle.attributeValue("EA_DIAG"), QVariant(QStringLiteral("ok")));
        QCOMPARE(describeLeaves(snapshot->leaves), describeLeaves({GraphNode::storedAttribute("EA_IN")}));
        QCOMPARE(snapshot->inputFingerprint.size(), InputFingerprintSize);
        QCOMPARE(snapshot->inputFingerprint, inputFingerprint(snapshot->leaves, w.source.state, &w.reg.prefs));

        // Twice the same, and still nothing changed
        QVERIFY(sameContent(*engine.exportResult("expA"), *snapshot));
        QVERIFY(EngineCounters(engine) == before);
    }
    {
        // A rejection is an Ok result with a reason
        Pair w;
        w.source.state.setAttribute("EA_IN", -1);
        QCOMPARE(w.source.engine.request("expA").status, ResultStatus::Ok);
        const std::optional<StoredCalculationResult> snapshot = w.source.engine.exportResult("expA");
        QVERIFY(snapshot.has_value());
        QCOMPARE(snapshot->detail, QStringLiteral("negative input"));
        QCOMPARE(snapshot->bundle.reason(), QStringLiteral("negative input"));
        QVERIFY(snapshot->bundle.setOutputs() == QList<DependencyKey>({attr("EA_DIAG")}));
        QCOMPARE(snapshot->bundle.attributeValue("EA_DIAG"), QVariant(QStringLiteral("rejected")));
        QVERIFY(!snapshot->bundle.isAvailable(attr("EA1")));
        QVERIFY(!snapshot->bundle.isAvailable(attr("EA2")));
    }
    {
        // Failed and MissingInput are cached, and not exported
        Pair w;
        QCOMPARE(w.source.engine.request("thrower").status, ResultStatus::Failed);
        QVERIFY(!w.source.engine.exportResult("thrower").has_value());
        w.target.state.removeAttribute("EA_IN");
        QCOMPARE(w.target.engine.request("expA").status, ResultStatus::MissingInput);
        QVERIFY(!w.target.engine.exportResult("expA").has_value());
    }
}

void CalcEngineRestoreTest::exportLeaves()
{
    Pair w;
    CalculationEngine &engine = w.source.engine;
    w.source.state.setMeasurement("S", "m", {1.0, 2.0, 3.0}, "u");

    QCOMPARE(engine.request("expA").status, ResultStatus::Ok);
    QCOMPARE(engine.request("expB").status, ResultStatus::Ok);
    QCOMPARE(engine.attribute("EB1"), QVariant(18));
    QCOMPARE(describeLeaves(engine.exportResult("expA")->leaves),
             describeLeaves({GraphNode::storedAttribute("EA_IN")}));
    // EA2 was looked at and is absent from the state; EA_IN is reached through
    // expA's result
    QVERIFY(!w.source.state.hasStoredAttribute("EA2"));
    QCOMPARE(describeLeaves(engine.exportResult("expB")->leaves),
             describeLeaves({GraphNode::storedAttribute("EA2"), GraphNode::storedAttribute("EA_IN"),
                             GraphNode::storedAttribute("EB_IN")}));

    QCOMPARE(engine.request("withPref").status, ResultStatus::Ok);
    QCOMPARE(engine.attribute("WP"), QVariant(405));
    QCOMPARE(describeLeaves(engine.exportResult("withPref")->leaves),
             describeLeaves({GraphNode::storedAttribute("EA_IN"), GraphNode::preference("p")}));

    // No source conversions registered: passthrough notes both source leaves
    QCOMPARE(engine.request("measExplicit").status, ResultStatus::Ok);
    QCOMPARE(engine.attribute("MS"), QVariant(3));
    QCOMPARE(describeLeaves(engine.exportResult("measExplicit")->leaves),
             describeLeaves({GraphNode::sourceMeasurement("S", "m"), GraphNode::sourceUnit("S", "m")}));
}

void CalcEngineRestoreTest::restoreIntoFreshEngine()
{
    Pair w;
    for (Session *s : {&w.source, &w.target}) {
        QVERIFY(!s->engine.isAvailable(attr("EA1")));
        QVERIFY(!s->engine.isAvailable(attr("DA")));
    }

    const CalculationEngine::RequestOutcome requested = w.source.engine.request("expA");
    QCOMPARE(requested.status, ResultStatus::Ok);
    const std::optional<StoredCalculationResult> snapshot = w.source.engine.exportResult("expA");
    QVERIFY(snapshot.has_value());

    const Restore outcome = w.target.engine.restoreResult(*snapshot);
    QCOMPARE(outcome.kind, Restore::Kind::Restored);
    QCOMPARE(outcome.staleCheck, StaleCheck::None);
    QCOMPARE(outcome.status, ResultStatus::Ok);
    QCOMPARE(outcome.invalidated, Names({attr("EA1"), attr("DA")}));
    QCOMPARE(outcome.invalidated, requested.invalidated);
    QVERIFY(w.target.broadcasts.isEmpty());     // returned to the caller, not broadcast

    // No run, no ticket (before any read: DA below is an on-demand run)
    CalculationEngine &target = w.target.engine;
    QCOMPARE(target.runCount("expA"), 0);
    QCOMPARE(target.totalRunCount(), 0);
    QCOMPARE(target.preparedCount(), 0);

    QCOMPARE(target.attribute("EA1"), QVariant(5));
    QCOMPARE(target.attribute("DA"), QVariant(105));
    QCOMPARE(target.resultStatus("expA"), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(target.resultDetail("expA"), w.source.engine.resultDetail("expA"));
    QCOMPARE(target.dependenciesOf(GraphNode::result("expA")),
             w.source.engine.dependenciesOf(GraphNode::result("expA")));
    QCOMPARE(target.dependenciesOf(GraphNode::result("expA")),
             QSet<GraphNode>({GraphNode::resolution(attr("EA_IN"))}));
    QCOMPARE(target.runCount("expA"), 0);
    QVERIFY(target.readiness("expA").state == CalculationReadiness::State::Done);

    // The same reads leave the same graph behind
    for (Session *s : {&w.source, &w.target}) {
        for (const DependencyKey &name : Synthetic::explicitNames())
            s->engine.isAvailable(name);
    }
    QCOMPARE(target.edgeCount(), w.source.engine.edgeCount());
    QCOMPARE(target.cachedNodeCount(), w.source.engine.cachedNodeCount());
    // The on-demand runs are the same; the one run missing is expA's
    QCOMPARE(target.totalRunCount(), w.source.engine.totalRunCount() - 1);

    const std::optional<StoredCalculationResult> again = target.exportResult("expA");
    QVERIFY(again.has_value());
    QVERIFY(sameContent(*again, *snapshot));

    // The restored values are those a recomputation gives
    QVERIFY(target.verifyAgainstFresh(Synthetic::explicitNames()).isEmpty());
    QCOMPARE(target.runCount("expA"), 0);
}

void CalcEngineRestoreTest::restoreRejection()
{
    Pair w;
    w.source.state.setAttribute("EA_IN", -1);
    w.target.state.setAttribute("EA_IN", -1);

    QCOMPARE(w.source.engine.request("expA").status, ResultStatus::Ok);
    const std::optional<StoredCalculationResult> snapshot = w.source.engine.exportResult("expA");
    QVERIFY(snapshot.has_value());

    const Restore outcome = w.target.engine.restoreResult(*snapshot);
    QCOMPARE(outcome.kind, Restore::Kind::Restored);
    QCOMPARE(w.target.engine.resultDetail("expA"), QStringLiteral("negative input"));
    QCOMPARE(w.target.engine.attribute("EA_DIAG"), QVariant(QStringLiteral("rejected")));
    QVERIFY(!w.target.engine.isAvailable(attr("EA1")));

    const BlockerReport report = w.target.engine.blockers(attr("EA1"));
    QVERIFY(report.state == BlockerReport::State::NotProduced);
    QVERIFY(report.blockers.isEmpty());
    QCOMPARE(report.notProduced.size(), 1);
    QCOMPARE(report.notProduced.first().calculation.instanceId, QStringLiteral("expA"));
    QCOMPARE(report.notProduced.first().status, ResultStatus::Ok);
    QCOMPARE(report.notProduced.first().detail, QStringLiteral("negative input"));
    QCOMPARE(reportText(report), reportText(w.source.engine.blockers(attr("EA1"))));
    QCOMPARE(reportText(w.target.engine.blockers(attr("DA"))), reportText(w.source.engine.blockers(attr("DA"))));
}

void CalcEngineRestoreTest::restoreNeverReplaces()
{
    Pair w;
    QCOMPARE(w.source.engine.request("expA").status, ResultStatus::Ok);
    const std::optional<StoredCalculationResult> snapshot = w.source.engine.exportResult("expA");
    QVERIFY(snapshot.has_value());

    StoredCalculationResult altered = *snapshot;
    altered.bundle = CalculationResult()
                         .setAttribute("EA1", 999)
                         .setAttribute("EA2", 8)
                         .setAttribute("EA_DIAG", QStringLiteral("ok"));
    const Restore outcome = w.source.engine.restoreResult(altered);
    QCOMPARE(outcome.kind, Restore::Kind::AlreadyInstalled);
    QCOMPARE(outcome.status, ResultStatus::Ok);
    QVERIFY(outcome.invalidated.isEmpty());
    QCOMPARE(w.source.engine.attribute("EA1"), QVariant(5));
    QCOMPARE(w.source.engine.runCount("expA"), 1);

    // A cached MissingInput is a requested result too
    w.target.state.removeAttribute("EA_IN");
    QCOMPARE(w.target.engine.request("expA").status, ResultStatus::MissingInput);
    const int cached = w.target.engine.cachedNodeCount();
    const Restore missing = w.target.engine.restoreResult(*snapshot);
    QCOMPARE(missing.kind, Restore::Kind::AlreadyInstalled);
    QCOMPARE(missing.status, ResultStatus::MissingInput);
    QCOMPARE(w.target.engine.resultStatus("expA"), std::optional<ResultStatus>(ResultStatus::MissingInput));
    QCOMPARE(w.target.engine.cachedNodeCount(), cached);
}

void CalcEngineRestoreTest::restoreStaleChecks_data()
{
    QTest::addColumn<QString>("row");
    QTest::addColumn<int>("check");
    QTest::addColumn<int>("status");

    QTest::newRow("result version") << "resultVersion" << int(StaleCheck::ResultVersion)
                                    << int(ResultStatus::NotRequested);
    QTest::newRow("undeclared output") << "undeclaredOutput" << int(StaleCheck::Bundle)
                                       << int(ResultStatus::NotRequested);
    QTest::newRow("detail is not the reason") << "detailNotReason" << int(StaleCheck::Bundle)
                                              << int(ResultStatus::NotRequested);
    QTest::newRow("input absent") << "inputAbsent" << int(StaleCheck::InputsUnavailable)
                                  << int(ResultStatus::MissingInput);
    QTest::newRow("upstream not installed") << "upstreamNotInstalled" << int(StaleCheck::InputsUnavailable)
                                            << int(ResultStatus::MissingInput);
    QTest::newRow("leaves") << "leaves" << int(StaleCheck::Leaves) << int(ResultStatus::NotRequested);
    QTest::newRow("fingerprint") << "fingerprint" << int(StaleCheck::Fingerprint)
                                 << int(ResultStatus::NotRequested);
}

// Each check is reached, installs nothing, and leaves the calculation "not
// requested": no run, not Done, its outputs unavailable.
void CalcEngineRestoreTest::restoreStaleChecks()
{
    QFETCH(QString, row);
    QFETCH(int, check);
    QFETCH(int, status);

    const bool byVersion = row == QStringLiteral("resultVersion");
    const bool chained = row == QStringLiteral("upstreamNotInstalled") || row == QStringLiteral("leaves");
    const CalculationId id = byVersion ? QStringLiteral("verA") : chained ? QStringLiteral("expB") : QStringLiteral("expA");
    const DependencyKey output = chained ? attr("EB1") : attr("EA1");

    // Separate registries, so that the ResultVersion row can give the same id
    // two result versions.
    Registry sourceRegistry(byVersion ? Registrar([](CalculationRegistry &r) { r.registerCalculation(versioned("v1")); })
                                      : Registrar(registerStandard));
    Registry targetRegistry(byVersion ? Registrar([](CalculationRegistry &r) { r.registerCalculation(versioned("v2")); })
                                      : Registrar(registerStandard));
    Session source(sourceRegistry);
    Session target(targetRegistry);

    if (chained)
        QCOMPARE(source.engine.request("expA").status, ResultStatus::Ok);
    QCOMPARE(source.engine.request(id).status, ResultStatus::Ok);
    std::optional<StoredCalculationResult> snapshot = source.engine.exportResult(id);
    QVERIFY(snapshot.has_value());
    QCOMPARE(snapshot->resultVersion, byVersion ? QStringLiteral("v1") : QString());

    if (row == QStringLiteral("undeclaredOutput"))
        snapshot->bundle.setAttribute("NOT_DECLARED", 1);
    else if (row == QStringLiteral("detailNotReason"))
        snapshot->detail = QStringLiteral("another text");
    else if (row == QStringLiteral("inputAbsent"))
        target.state.removeAttribute("EA_IN");
    else if (row == QStringLiteral("leaves"))
        target.state.setAttribute("EA2", 8);     // stored: expB no longer reaches expA
    else if (row == QStringLiteral("fingerprint"))
        target.state.setAttribute("EA_IN", 5);

    QVERIFY(!target.engine.isAvailable(output));

    const Restore outcome = target.engine.restoreResult(*snapshot);
    QCOMPARE(outcome.kind, Restore::Kind::Stale);
    QCOMPARE(int(outcome.staleCheck), check);
    QCOMPARE(int(outcome.status), status);
    // The checks before gathering touch nothing; the ones after it report what
    // gathering dropped.
    if (check == int(StaleCheck::ResultVersion) || check == int(StaleCheck::Bundle))
        QVERIFY(outcome.invalidated.isEmpty());
    else
        QVERIFY(outcome.invalidated.contains(output));

    QVERIFY(isNotRun(target.engine.resultStatus(id)));
    QVERIFY(target.engine.readiness(id).state != CalculationReadiness::State::Done);
    QCOMPARE(target.engine.runCount(id), 0);
    QCOMPARE(target.engine.totalRunCount(), 0);
    QCOMPARE(target.engine.preparedCount(), 0);
    QVERIFY(!target.engine.isAvailable(output));
    QVERIFY(!target.engine.exportResult(id).has_value());
    QVERIFY(target.events.isEmpty());
}

// A leaf reached through an explicit result: the downstream calculation
// restores only once the upstream one is installed.
void CalcEngineRestoreTest::restoreChain()
{
    Pair w;
    QCOMPARE(w.source.engine.request("expA").status, ResultStatus::Ok);
    QCOMPARE(w.source.engine.request("expB").status, ResultStatus::Ok);
    const std::optional<StoredCalculationResult> a = w.source.engine.exportResult("expA");
    const std::optional<StoredCalculationResult> b = w.source.engine.exportResult("expB");
    QVERIFY(a.has_value() && b.has_value());

    CalculationEngine &target = w.target.engine;
    const Restore early = target.restoreResult(*b);
    QCOMPARE(early.kind, Restore::Kind::Stale);
    QCOMPARE(early.staleCheck, StaleCheck::InputsUnavailable);
    QCOMPARE(early.status, ResultStatus::MissingInput);
    QVERIFY(isNotRun(target.resultStatus("expB")));

    QCOMPARE(target.restoreResult(*a).kind, Restore::Kind::Restored);
    const Restore late = target.restoreResult(*b);
    QCOMPARE(late.kind, Restore::Kind::Restored);
    QCOMPARE(late.status, ResultStatus::Ok);
    QCOMPARE(target.totalRunCount(), 0);

    QCOMPARE(target.attribute("EB1"), QVariant(18));
    QCOMPARE(target.attribute("DB"), QVariant(19));
    QCOMPARE(target.runCount("expA"), 0);
    QCOMPARE(target.runCount("expB"), 0);
    QVERIFY(sameContent(*target.exportResult("expB"), *b));
    QCOMPARE(target.dependenciesOf(GraphNode::result("expB")),
             w.source.engine.dependenciesOf(GraphNode::result("expB")));
    QVERIFY(target.verifyAgainstFresh(Synthetic::explicitNames()).isEmpty());
}

// The session file's text is what is fingerprinted: an attribute that reloads
// as text matches the number it was published with.
void CalcEngineRestoreTest::restoreIgnoresAttributeType()
{
    Pair w;
    QCOMPARE(w.source.engine.request("expA").status, ResultStatus::Ok);
    const std::optional<StoredCalculationResult> snapshot = w.source.engine.exportResult("expA");
    QVERIFY(snapshot.has_value());

    w.target.state.setAttribute("EA_IN", QVariant(QStringLiteral("4")));
    const Restore outcome = w.target.engine.restoreResult(*snapshot);
    QCOMPARE(outcome.kind, Restore::Kind::Restored);
    QCOMPARE(w.target.engine.attribute("EA1"), QVariant(5));
    QCOMPARE(w.target.engine.runCount("expA"), 0);
}

void CalcEngineRestoreTest::restoreNotFound()
{
    Pair w;
    QCOMPARE(w.source.engine.request("expA").status, ResultStatus::Ok);
    const std::optional<StoredCalculationResult> snapshot = w.source.engine.exportResult("expA");
    QVERIFY(snapshot.has_value());

    CalculationEngine &target = w.target.engine;
    QVERIFY(!target.isAvailable(attr("EA1")));
    const int cached = target.cachedNodeCount();

    const struct {
        const char *id;
        Restore::Kind kind;
    } cases[] = {
        {"nope", Restore::Kind::NotFound},
        {"neg", Restore::Kind::NotFound},           // a family of the shared world
        {"neg#neg:X", Restore::Kind::NotFound},     // an instance id is not a plain id
        {"derivA", Restore::Kind::NotExplicit},
    };
    for (const auto &c : cases) {
        StoredCalculationResult other = *snapshot;
        other.calculationId = QString::fromLatin1(c.id);
        const Restore outcome = target.restoreResult(other);
        QVERIFY2(outcome.kind == c.kind, c.id);
        QVERIFY(outcome.staleCheck == StaleCheck::None);
        QVERIFY(outcome.status == ResultStatus::NotRequested);
        QVERIFY(outcome.invalidated.isEmpty());
        QCOMPARE(target.cachedNodeCount(), cached);
    }
    QVERIFY(!target.isAvailable(attr("EA1")));
    QCOMPARE(target.totalRunCount(), 0);
}

void CalcEngineRestoreTest::restoreBeatsOutstandingTicket()
{
    QFETCH(int, mode);
    Pair w;
    QCOMPARE(w.source.engine.request("expA").status, ResultStatus::Ok);
    const std::optional<StoredCalculationResult> snapshot = w.source.engine.exportResult("expA");
    QVERIFY(snapshot.has_value());

    CalculationEngine &target = w.target.engine;
    Prepare prepared = target.prepare("expA");
    QCOMPARE(prepared.kind, Prepare::Kind::Ready);
    QVERIFY(prepared.ticket);

    ComputedCalculation computed;
    {
        // The restore happens while compute() may still be running.
        ComputeRun run(ComputeMode(mode), *prepared.ticket);
        QCOMPARE(target.restoreResult(*snapshot).kind, Restore::Kind::Restored);
        computed = run.finish();
    }
    QCOMPARE(computed.kind, ComputedCalculation::Kind::Completed);

    const PublishOutcome published = prepared.ticket->publish(std::move(computed));
    QCOMPARE(published.kind, PublishOutcome::Kind::RefusedStale);
    QCOMPARE(published.reason, PublishOutcome::Reason::AlreadyPublished);

    QCOMPARE(target.attribute("EA1"), QVariant(5));
    QCOMPARE(target.resultStatus("expA"), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(target.runCount("expA"), 0);
    QCOMPARE(target.preparedCount(), 0);
    QVERIFY(w.target.events.isEmpty());
}

void CalcEngineRestoreTest::restoredResultInvalidatesLikePublished()
{
    Pair w;
    QCOMPARE(w.source.engine.request("expA").status, ResultStatus::Ok);
    const std::optional<StoredCalculationResult> snapshot = w.source.engine.exportResult("expA");
    QVERIFY(snapshot.has_value());
    QCOMPARE(w.target.engine.restoreResult(*snapshot).kind, Restore::Kind::Restored);

    for (Session *s : {&w.source, &w.target}) {
        for (const DependencyKey &name : Synthetic::explicitNames())
            s->engine.isAvailable(name);
    }

    const Names fromPublished = w.source.state.setAttribute(w.source.engine, "EA_IN", 7);
    const Names fromRestored = w.target.state.setAttribute(w.target.engine, "EA_IN", 7);
    QCOMPARE(fromRestored, fromPublished);
    for (const char *name : {"EA_IN", "EA1", "EA2", "EA_DIAG", "DA", "DDA"})
        QVERIFY2(fromRestored.contains(attr(name)), name);

    for (Session *s : {&w.source, &w.target}) {
        QVERIFY(!s->engine.isAvailable(attr("EA1")));
        QVERIFY(isNotRun(s->engine.resultStatus("expA")));
        QVERIFY(!s->engine.exportResult("expA").has_value());
    }
    QCOMPARE(describeEvents(w.source.events),
             QStringList({"Installed expA Ok", "DroppedByInputChange expA Ok"}));
    QCOMPARE(describeEvents(w.target.events), QStringList({"DroppedByInputChange expA Ok"}));
}

void CalcEngineRestoreTest::restoreNaNPayloadAndSignedZero()
{
    Registry reg;
    Session source(reg);
    Session sameValues(reg);
    Session otherZero(reg);

    source.state.setMeasurement("S", "m", {1.0, qQNaN(), -0.0}, "u");
    sameValues.state.setMeasurement("S", "m", {1.0, fromBits(Q_UINT64_C(0xFFF8000000000123)), -0.0}, "u");
    otherZero.state.setMeasurement("S", "m", {1.0, qQNaN(), +0.0}, "u");

    QCOMPARE(source.engine.request("measExplicit").status, ResultStatus::Ok);
    QCOMPARE(source.engine.attribute("MS"), QVariant(3));
    const std::optional<StoredCalculationResult> snapshot = source.engine.exportResult("measExplicit");
    QVERIFY(snapshot.has_value());

    const Restore restored = sameValues.engine.restoreResult(*snapshot);
    QCOMPARE(restored.kind, Restore::Kind::Restored);
    QCOMPARE(sameValues.engine.attribute("MS"), QVariant(3));

    const Restore stale = otherZero.engine.restoreResult(*snapshot);
    QCOMPARE(stale.kind, Restore::Kind::Stale);
    QCOMPARE(stale.staleCheck, StaleCheck::Fingerprint);
    QVERIFY(!otherZero.engine.isAvailable(attr("MS")));
}

// sameContent() compares attribute values bit-exactly, as it does samples:
// NaN equals a NaN with the same bits, -0.0 differs from +0.0, and a value
// differs from the same number held in another metatype.
void CalcEngineRestoreTest::sameContentBitExactAttributes()
{
    const auto snapshot = [](const QVariant &value) {
        StoredCalculationResult s;
        s.calculationId = QStringLiteral("calc");
        s.bundle.setAttribute("A", value);
        s.bundle.setMeasurement("S", "m", {1.0, qQNaN(), -0.0}, "u");
        return s;
    };
    const auto same = [&](const QVariant &a, const QVariant &b) {
        return sameContent(snapshot(a), snapshot(b));
    };
    const double nan = fromBits(Q_UINT64_C(0x7FF8000000000000));

    QVERIFY(same(QVariant(nan), QVariant(nan)));                // QVariant == says unequal
    QVERIFY(same(QVariant(-0.0), QVariant(-0.0)));
    QVERIFY(same(QVariant(1.5), QVariant(1.5)));
    QVERIFY(same(QVariant(QStringLiteral("x")), QVariant(QStringLiteral("x"))));

    QVERIFY(!same(QVariant(+0.0), QVariant(-0.0)));             // QVariant == says equal
    QVERIFY(!same(QVariant(-0.0), QVariant(+0.0)));
    // Quiet NaNs differing in payload and in sign; copies preserve the bits.
    QVERIFY(!same(QVariant(nan), QVariant(fromBits(Q_UINT64_C(0x7FF8000000000123)))));
    QVERIFY(!same(QVariant(nan), QVariant(fromBits(Q_UINT64_C(0xFFF8000000000000)))));
    QVERIFY(!same(QVariant(1), QVariant(1.0)));                 // metatype differs
    QVERIFY(!same(QVariant(1.5f), QVariant(1.5)));
    QVERIFY(!same(QVariant(QStringLiteral("x")), QVariant(QStringLiteral("X"))));

    // A float compares by its own bits.
    QVERIFY(same(QVariant(std::numeric_limits<float>::quiet_NaN()),
                 QVariant(std::numeric_limits<float>::quiet_NaN())));
    QVERIFY(!same(QVariant(0.0f), QVariant(-0.0f)));
}

void CalcEngineRestoreTest::installedOnSyncAndAsync()
{
    QFETCH(int, mode);
    Pair w;

    // Synchronous
    QCOMPARE(w.source.engine.request("expA").status, ResultStatus::Ok);
    QCOMPARE(describeEvents(w.source.takeEvents()), QStringList({"Installed expA Ok"}));
    QCOMPARE(w.source.engine.request("expA").status, ResultStatus::Ok);    // valid: not recomputed
    QVERIFY(w.source.events.isEmpty());

    // Asynchronous: nothing at prepare, Installed at publish
    Prepare prepared = w.target.engine.prepare("expA");
    QCOMPARE(prepared.kind, Prepare::Kind::Ready);
    QVERIFY(w.target.events.isEmpty());
    ComputedCalculation computed = computeOn(ComputeMode(mode), *prepared.ticket);
    QVERIFY(w.target.events.isEmpty());
    const PublishOutcome published = prepared.ticket->publish(std::move(computed));
    QCOMPARE(published.kind, PublishOutcome::Kind::Published);
    QCOMPARE(describeEvents(w.target.takeEvents()), QStringList({"Installed expA Ok"}));

    // A refused publish installs nothing
    Session third(w.reg);
    Prepare refused = third.engine.prepare("expA");
    QCOMPARE(refused.kind, Prepare::Kind::Ready);
    ComputedCalculation stale = computeOn(ComputeMode(mode), *refused.ticket);
    third.state.setAttribute(third.engine, "EA_IN", 5);
    const PublishOutcome refusedOutcome = refused.ticket->publish(std::move(stale));
    QCOMPARE(refusedOutcome.kind, PublishOutcome::Kind::RefusedStale);
    QCOMPARE(refusedOutcome.reason, PublishOutcome::Reason::InputsChanged);
    QVERIFY(third.events.isEmpty());
}

void CalcEngineRestoreTest::installedForEveryStatus()
{
    Pair w;

    // prepare()'s NothingToRun installs MissingInput
    w.source.state.removeAttribute("EA_IN");
    const Prepare prepared = w.source.engine.prepare("expA");
    QCOMPARE(prepared.kind, Prepare::Kind::NothingToRun);
    QCOMPARE(describeEvents(w.source.takeEvents()), QStringList({"Installed expA MissingInput"}));

    // request() of a thrower installs Failed
    QCOMPARE(w.target.engine.request("thrower").status, ResultStatus::Failed);
    QCOMPARE(describeEvents(w.target.takeEvents()), QStringList({"Installed thrower Failed"}));

    // request() of an on-demand calculation is never reported
    QCOMPARE(w.target.engine.request("derivA").status, ResultStatus::MissingInput);
    QVERIFY(w.target.events.isEmpty());
    QCOMPARE(w.target.engine.request("expA").status, ResultStatus::Ok);
    QCOMPARE(describeEvents(w.target.takeEvents()), QStringList({"Installed expA Ok"}));
    QCOMPARE(w.target.engine.request("derivA").status, ResultStatus::Ok);
    QCOMPARE(w.target.engine.attribute("DA"), QVariant(105));
    QVERIFY(w.target.events.isEmpty());
}

void CalcEngineRestoreTest::restoreIsNotAnInstall()
{
    Pair w;
    QCOMPARE(w.source.engine.request("expA").status, ResultStatus::Ok);
    const std::optional<StoredCalculationResult> snapshot = w.source.engine.exportResult("expA");
    QVERIFY(snapshot.has_value());

    QVERIFY(!w.target.engine.isAvailable(attr("EA1")));
    QCOMPARE(w.target.engine.restoreResult(*snapshot).kind, Restore::Kind::Restored);
    QCOMPARE(w.target.engine.attribute("EA1"), QVariant(5));
    QVERIFY(w.target.events.isEmpty());
    QVERIFY(w.target.log.isEmpty());
}

void CalcEngineRestoreTest::droppedByInputChange()
{
    Pair w;
    Session &s = w.source;
    QCOMPARE(s.engine.request("expA").status, ResultStatus::Ok);
    QCOMPARE(s.engine.request("expB").status, ResultStatus::Ok);
    QCOMPARE(s.takeEvents().size(), 2);

    // Both drops are delivered before setAttribute returns; their relative
    // order follows the invalidation walk and is not specified.
    s.state.setAttribute(s.engine, "EA_IN", 7);
    const QStringList events = describeEvents(s.takeEvents());
    QCOMPARE(events.size(), 2);
    QCOMPARE(QSet<QString>(events.cbegin(), events.cend()),
             QSet<QString>({"DroppedByInputChange expA Ok", "DroppedByInputChange expB Ok"}));

    // An unrelated change drops nothing
    QCOMPARE(s.engine.request("expA").status, ResultStatus::Ok);
    s.takeEvents();
    s.state.setAttribute(s.engine, "UNRELATED", 1);
    QVERIFY(s.events.isEmpty());
}

void CalcEngineRestoreTest::droppedByPreferenceAndSource()
{
    Pair w;
    {
        // A declared preference: after the names listener has its set
        Session &s = w.source;
        QCOMPARE(s.engine.request("withPref").status, ResultStatus::Ok);
        QCOMPARE(s.engine.attribute("WP"), QVariant(405));
        s.takeEvents();
        s.log.clear();

        w.reg.prefs.set(w.reg.registry, "p", 6);
        QCOMPARE(describeEvents(s.takeEvents()), QStringList({"DroppedByInputChange withPref Ok"}));
        QCOMPARE(s.log, QStringList({"names", "explicit withPref"}));
        QVERIFY(s.broadcasts.last().contains(attr("WP")));

        // An undeclared one reaches nothing
        s.log.clear();
        w.reg.prefs.set(w.reg.registry, "q", 1);
        QVERIFY(s.events.isEmpty());
        QVERIFY(s.log.isEmpty());
    }
    {
        // A source measurement and a source unit
        Session &s = w.target;
        s.state.setMeasurement("S", "m", {1.0, 2.0, 3.0}, "u");
        QCOMPARE(s.engine.request("measExplicit").status, ResultStatus::Ok);
        QCOMPARE(s.engine.attribute("MS"), QVariant(3));
        s.takeEvents();

        s.state.setMeasurement(s.engine, "S", "m", {1.0, 2.0, 3.0, 4.0}, "u");
        QCOMPARE(describeEvents(s.takeEvents()), QStringList({"DroppedByInputChange measExplicit Ok"}));

        QCOMPARE(s.engine.request("measExplicit").status, ResultStatus::Ok);
        QCOMPARE(s.engine.attribute("MS"), QVariant(4));
        s.takeEvents();
        s.state.setUnit(s.engine, "S", "m", "v");
        QCOMPARE(describeEvents(s.takeEvents()), QStringList({"DroppedByInputChange measExplicit Ok"}));
    }
}

// A requested result that used another calculation's "not requested" answer
// has had an input change when that calculation is requested or restored.
void CalcEngineRestoreTest::droppedByRequestOfUpstream()
{
    Pair w;
    QCOMPARE(w.source.engine.request("expA").status, ResultStatus::Ok);
    const std::optional<StoredCalculationResult> snapshot = w.source.engine.exportResult("expA");
    QVERIFY(snapshot.has_value());

    {
        Session s(w.reg);
        QCOMPARE(s.engine.request("expB").status, ResultStatus::MissingInput);
        QCOMPARE(describeEvents(s.takeEvents()), QStringList({"Installed expB MissingInput"}));
        QCOMPARE(s.engine.request("expA").status, ResultStatus::Ok);
        QCOMPARE(describeEvents(s.takeEvents()),
                 QStringList({"DroppedByInputChange expB MissingInput", "Installed expA Ok"}));
    }
    {
        // The same through a restore: the drop, and no Installed
        Session &s = w.target;
        QCOMPARE(s.engine.request("expB").status, ResultStatus::MissingInput);
        s.takeEvents();
        QCOMPARE(s.engine.restoreResult(*snapshot).kind, Restore::Kind::Restored);
        QCOMPARE(describeEvents(s.takeEvents()), QStringList({"DroppedByInputChange expB MissingInput"}));
    }
}

void CalcEngineRestoreTest::noDropEventWithoutInputChange()
{
    Pair w;
    Session &s = w.source;

    // clear()
    QCOMPARE(s.engine.request("expA").status, ResultStatus::Ok);
    s.takeEvents();
    s.engine.clear();
    QVERIFY(isNotRun(s.engine.resultStatus("expA")));
    QVERIFY(s.events.isEmpty());

    // A registration that declares expA's input, then its removal: both drop
    // the result, neither is an input change
    QCOMPARE(s.engine.request("expA").status, ResultStatus::Ok);
    s.takeEvents();
    QVERIFY(w.reg.registry.registerCalculation(altIn()));
    QVERIFY(!s.engine.resultStatus("expA").has_value());
    QVERIFY(s.events.isEmpty());

    QCOMPARE(s.engine.request("expA").status, ResultStatus::Ok);
    QCOMPARE(s.engine.attribute("EA1"), QVariant(5));      // the stored EA_IN still wins
    s.takeEvents();
    QVERIFY(w.reg.registry.unregister("altIn"));
    QVERIFY(!s.engine.resultStatus("expA").has_value());
    QVERIFY(s.events.isEmpty());

    // The engine's destruction
    QList<Event> events;
    FakeSessionState state;
    state.setAttribute("EA_IN", 4);
    auto engine = std::make_unique<CalculationEngine>(&state, &w.reg.registry);
    engine->setExplicitResultListener([&events](const Event &event) { events.append(event); });
    QCOMPARE(engine->request("expA").status, ResultStatus::Ok);
    QCOMPARE(describeEvents(events), QStringList({"Installed expA Ok"}));
    events.clear();
    engine.reset();
    QVERIFY(events.isEmpty());
}

FLYSIGHT_TEST_MAIN(CalcEngineRestoreTest)
#include "tst_calcengine_restore.moc"
