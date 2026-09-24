// Idempotency oracle (acceptance 10): after any sequence of reads,
// edits, preference changes, and registry changes, the value returned for every
// name equals the value obtained from a fresh evaluation with empty caches.
//
// Randomized but reproducible: std::mt19937 with a literal seed, reduced with
// `rng() % n` only (std::uniform_int_distribution differs between standard
// libraries). This is the one place where a computed value is the expectation,
// because the rule under test IS "equal to a fresh evaluation"; every sequence
// also starts with literal checkpoints.
//
// randomizedExplicitSequences adds explicit calculations to the mix: blocker
// inspection, synchronous requests, asynchronous requests (compute inline; the
// threads are tst_calcengine_async's business), asynchronous requests whose
// input changes before publish, and registry changes of candidates the
// explicit calculations look up. None of it may disturb the invariant, and an
// explicit calculation may run only in a request or publish step.
//
// registryChangePrecision pins, row by row, which cached answers a registry
// change keeps (a candidate behind the provider or behind the session's own
// data, a removed candidate that was passed over) and which it drops, and that
// a kept answer is served without a run and equals a fresh evaluation.

#include <cstdio>
#include <limits>
#include <random>

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

// The shared world (acyclic, plus the single P/R ring) and the tangle world
// (overlapping rings, where a cached answer shaped by one entry point would be
// wrong for another).
QList<DependencyKey> allNames()
{
    QList<DependencyKey> names = {attr("X"), attr("Y"), attr("Z"), attr("W"), attr("X2"), attr("Y2"),
                                  attr("neg:A"), attr("neg:X"), attr("neg:OY"), attr("neg:K3"),
                                  measKey("S", "d"), measKey("S", "m")};
    names.append(Synthetic::tangleNames());
    return names;
}

// The cycles in the synthetic worlds are reported every time a ring is met;
// thousands of identical warnings would bury a real failure.
void quietHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (type == QtWarningMsg && message.contains(QLatin1String("dependency cycle")))
        return;
    fprintf(stderr, "%s\n", qPrintable(qFormatLogMessage(type, context, message)));
}

struct Session {
    FakeSessionState state;
    CalculationEngine engine;

    explicit Session(CalculationRegistry &registry)
        : engine(&state, &registry)
    {
        state.setAttribute("A", 1);
        state.setAttribute("B", 2);
        state.setMeasurement("S", "m", {1.0, 2.0, 3.0});
    }
};

} // namespace

class CalcEngineOracleTest : public QObject {
    Q_OBJECT

private slots:
    void sameValueSemantics();
    void evaluateFreshLeavesEngineUntouched();
    void randomizedSequences_data();
    void randomizedSequences();
    void randomizedTopologies_data();
    void randomizedTopologies();
    void randomizedExplicitSequences_data();
    void randomizedExplicitSequences();
    void registryChangePrecision_data();
    void registryChangePrecision();
};

void CalcEngineOracleTest::sameValueSemantics()
{
    using Value = CalculationEngine::Value;
    const double nan = std::numeric_limits<double>::quiet_NaN();

    Value a, b;
    QVERIFY(CalculationEngine::sameValue(a, b));        // both unavailable

    a.available = b.available = true;
    a.samples = {1.0, nan, 3.0};
    b.samples = {1.0, nan, 3.0};
    QVERIFY(CalculationEngine::sameValue(a, b));        // NaN with identical bits is equal

    b.samples = {1.0, nan};
    QVERIFY(!CalculationEngine::sameValue(a, b));
    a.samples = {0.0};
    b.samples = {-0.0};
    QVERIFY(!CalculationEngine::sameValue(a, b));       // bit pattern, not ==
    b.samples = {0.0};
    QVERIFY(CalculationEngine::sameValue(a, b));

    b.unit = QStringLiteral("m");
    QVERIFY(!CalculationEngine::sameValue(a, b));
    b.unit.clear();
    a.attribute = 3;
    b.attribute = 4;
    QVERIFY(!CalculationEngine::sameValue(a, b));
    b.attribute = 3;
    QVERIFY(CalculationEngine::sameValue(a, b));
    b.available = false;
    QVERIFY(!CalculationEngine::sameValue(a, b));
}

void CalcEngineOracleTest::evaluateFreshLeavesEngineUntouched()
{
    CalculationRegistry registry;
    FakePreferenceProvider prefs;
    registry.setPreferenceProvider(&prefs);
    Synthetic::registerSharedWorld(registry);
    prefs.set("p", 5);
    Session s(registry);

    QCOMPARE(s.engine.attribute("X"), QVariant(3));
    const int runs = s.engine.totalRunCount();
    const int edges = s.engine.edgeCount();
    const int nodes = s.engine.cachedNodeCount();
    QCOMPARE(registry.enrolledEngineCount(), 1);

    // Names the real engine has and has not cached.
    const CalculationEngine::Value x = s.engine.evaluateFresh(attr("X"));
    QVERIFY(x.available);
    QCOMPARE(x.attribute, QVariant(3));
    const CalculationEngine::Value d = s.engine.evaluateFresh(measKey("S", "d"));
    QVERIFY(d.available);
    QCOMPARE(d.samples, (QVector<double>{9.0, 10.0, 11.0}));
    QCOMPARE(d.unit, QStringLiteral("u"));
    QVERIFY(!s.engine.evaluateFresh(attr("nothing")).available);

    QCOMPARE(s.engine.totalRunCount(), runs);
    QCOMPARE(s.engine.edgeCount(), edges);
    QCOMPARE(s.engine.cachedNodeCount(), nodes);
    QCOMPARE(s.engine.cachedState(measKey("S", "d")), CalculationEngine::CachedState::NotCached);
    QCOMPARE(s.engine.cycleCount(), 0);
    QCOMPARE(registry.enrolledEngineCount(), 1);

    // The oracle detects a stale cache: mutate the state without telling the engine.
    s.state.setAttribute("A", 100);
    QCOMPARE(s.engine.verifyAgainstFresh({attr("X"), attr("B")}), QList<DependencyKey>({attr("X")}));
    s.engine.attributeChanged("A");
    QVERIFY(s.engine.verifyAgainstFresh({attr("X"), attr("B")}).isEmpty());
}

void CalcEngineOracleTest::randomizedSequences_data()
{
    QTest::addColumn<int>("seed");
    for (int seed = 1; seed <= 25; ++seed)
        QTest::addRow("seed %d", seed) << seed;
}

// Acceptance 10 (idempotency): read order and cache contents never change the
// answer, across edits, preference changes, and registry changes, in two
// sessions sharing one registry.
void CalcEngineOracleTest::randomizedSequences()
{
    QFETCH(int, seed);
    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed));
    const auto pick = [&rng](int n) { return int(rng() % static_cast<unsigned>(n)); };

    CalculationRegistry registry;
    FakePreferenceProvider prefs;
    registry.setPreferenceProvider(&prefs);
    Synthetic::registerSharedWorld(registry);
    Synthetic::registerTangleWorld(registry);
    prefs.set("p", 5);

    Session first(registry), second(registry);
    Session *const sessions[2] = {&first, &second};
    const QList<DependencyKey> names = allNames();

    // Literal checkpoint before anything random happens. The second session
    // reads the tangle world in the opposite order.
    const QtMessageHandler previousHandler = qInstallMessageHandler(quietHandler);
    struct RestoreHandler {
        QtMessageHandler handler;
        ~RestoreHandler() { qInstallMessageHandler(handler); }
    } restore{previousHandler};

    const QList<std::pair<const char *, QVariant>> tangleCheckpoint = {
        {"OX", 100}, {"OY", QVariant()}, {"E1", 101}, {"E2", 7}, {"C1", 50}, {"A1", 1}, {"B1", 51},
        {"K1", 10}, {"K2", 20}, {"K3", 31}, {"neg:K3", -31}, {"neg:OY", QVariant()}};
    for (int i = 0; i < 2; ++i) {
        Session &s = *sessions[i];
        for (qsizetype k = 0; k < tangleCheckpoint.size(); ++k) {
            const auto &expected = tangleCheckpoint.at(i == 0 ? k : tangleCheckpoint.size() - 1 - k);
            QVERIFY2(s.engine.attribute(QString::fromLatin1(expected.first)) == expected.second,
                     (QByteArray("session ") + QByteArray::number(i) + " checkpoint " + expected.first).constData());
        }
        QVERIFY(s.engine.verifyAgainstFresh(names).isEmpty());
    }

    for (Session *session : sessions) {
        Session &s = *session;
        QCOMPARE(s.engine.attribute("X"), QVariant(3));
        QCOMPARE(s.engine.attribute("Y"), QVariant(8));
        QCOMPARE(s.engine.attribute("Z"), QVariant(6));
        QCOMPARE(s.engine.attribute("W"), QVariant(9));
        QCOMPARE(s.engine.attribute("X2"), QVariant(100));
        QCOMPARE(s.engine.attribute("Y2"), QVariant(200));
        QCOMPARE(s.engine.attribute("neg:A"), QVariant(-1));
        QCOMPARE(s.engine.measurement("S", "d"), (QVector<double>{9.0, 10.0, 11.0}));
    }

    // Storing a value for a name on a ring cuts the ring; removing it closes it.
    const QStringList editable = {"A", "B", "C", "X", "Z", "Y2", "OX", "OY", "A1", "C1", "K1", "K2"};
    const QStringList toggled = {"fallbackX", "wAlt", "neg", "oQ", "oS", "tC", "tA", "c2", "k3"};

    for (int step = 0; step < 400; ++step) {
        Session &s = *sessions[pick(2)];
        const int op = pick(100);
        const QByteArray where = QByteArray("seed ") + QByteArray::number(seed)
                               + " step " + QByteArray::number(step);

        if (op < 50) {
            // Read a random name; it must equal a fresh evaluation.
            const DependencyKey name = names.at(pick(int(names.size())));
            const QList<DependencyKey> mismatch = s.engine.verifyAgainstFresh({name});
            QVERIFY2(mismatch.isEmpty(), (where + " read of " + describe(name).toUtf8()).constData());
            continue;
        }

        const int runs0 = sessions[0]->engine.totalRunCount();
        const int runs1 = sessions[1]->engine.totalRunCount();

        if (op < 80) {
            const QString key = editable.at(pick(int(editable.size())));
            if (pick(3) == 0)
                s.state.removeAttribute(s.engine, key);
            else
                s.state.setAttribute(s.engine, key, pick(7));
        } else if (op < 88) {
            if (pick(3) == 0)
                s.state.removeMeasurement(s.engine, "S", "m");
            else
                s.state.setMeasurement(s.engine, "S", "m",
                                       {double(pick(7)), double(pick(7)), double(pick(7))});
        } else if (op < 95) {
            prefs.set(registry, "p", pick(3) == 0 ? QVariant() : QVariant(pick(7)));
        } else {
            // Unregister or re-register: also moves the candidate to the end.
            const QString id = toggled.at(pick(int(toggled.size())));
            if (registry.contains(id)) {
                QVERIFY2(registry.unregister(id, CalculationRegistry::Removal::Change), where.constData());
            } else if (id == QLatin1String("fallbackX")) {
                QVERIFY2(registry.registerCalculation(Synthetic::fallbackX()), where.constData());
            } else if (id == QLatin1String("wAlt")) {
                QVERIFY2(registry.registerCalculation(Synthetic::wAlt()), where.constData());
            } else if (id == QLatin1String("neg")) {
                QVERIFY2(registry.registerFamily(Synthetic::neg()), where.constData());
            } else {
                QVERIFY2(registry.registerCalculation(Synthetic::tangle(id)), where.constData());
            }
        }

        // No notification ever computes, in either session.
        QVERIFY2(sessions[0]->engine.totalRunCount() == runs0, where.constData());
        QVERIFY2(sessions[1]->engine.totalRunCount() == runs1, where.constData());
        QVERIFY2(s.engine.scopeDepth() == 0, where.constData());

        // Right after a change, when the caches are part dropped and part kept,
        // is where an answer left over from another entry point would show:
        // read a few names in a random order, in both sessions.
        for (Session *session : sessions) {
            for (int k = 0; k < 3; ++k) {
                const DependencyKey name = names.at(pick(int(names.size())));
                const QList<DependencyKey> mismatch = session->engine.verifyAgainstFresh({name});
                QVERIFY2(mismatch.isEmpty(),
                         (where + " read after change of " + describe(name).toUtf8()).constData());
            }
        }
    }

    for (Session *session : sessions) {
        Session &s = *session;
        const QList<DependencyKey> mismatch = s.engine.verifyAgainstFresh(names);
        QVERIFY2(mismatch.isEmpty(),
                 qPrintable(QStringLiteral("seed %1: %2 names differ from a fresh evaluation, first %3")
                                .arg(seed).arg(mismatch.size())
                                .arg(mismatch.isEmpty() ? QString() : describe(mismatch.first()))));
        QCOMPARE(s.engine.scopeDepth(), 0);
        QCOMPARE(s.engine.undeclaredReadCount(), 0);
    }
}

void CalcEngineOracleTest::randomizedTopologies_data()
{
    QTest::addColumn<int>("seed");
    for (int seed = 1; seed <= 300; ++seed)
        QTest::addRow("seed %d", seed) << seed;
}

// Acceptance 10 on graphs nobody designed: six names, each with one to three
// candidates that read up to two random other names, so most worlds
// contain several overlapping rings. Whatever the rings are, a warm engine must
// agree with a fresh evaluation after every read, in any order, across edits
// that cut and close rings and registry changes that remove candidates.
void CalcEngineOracleTest::randomizedTopologies()
{
    QFETCH(int, seed);
    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed));
    const auto pick = [&rng](int n) { return int(rng() % static_cast<unsigned>(n)); };

    const QtMessageHandler previousHandler = qInstallMessageHandler(quietHandler);
    struct RestoreHandler {
        QtMessageHandler handler;
        ~RestoreHandler() { qInstallMessageHandler(handler); }
    } restore{previousHandler};

    const int nameCount = 6;
    const auto key = [](int i) { return QStringLiteral("N%1").arg(i); };

    CalculationRegistry registry;
    QList<CalculationDescriptor> descriptors;
    for (int n = 0; n < nameCount; ++n) {
        const int candidates = 1 + pick(3);
        for (int c = 0; c < candidates; ++c) {
            const QString output = key(n);
            QStringList inputs;
            const int inputCount = pick(3);             // 0 = a constant
            for (int i = 0; i < inputCount; ++i) {
                // Any other name (the registry refuses a direct self-input).
                const QString input = key((n + 1 + pick(nameCount - 1)) % nameCount);
                if (!inputs.contains(input))
                    inputs.append(input);
            }
            const int constant = 1 + pick(9);

            CalculationDescriptor d;
            d.id = QStringLiteral("c%1_%2").arg(n).arg(c);
            for (const QString &input : std::as_const(inputs))
                d.inputs.append(CalcInput::attribute(input));
            d.outputs = {DependencyKey::attribute(output)};
            d.compute = [output, inputs, constant](const EvaluationContext &ctx) {
                int value = constant;
                for (const QString &input : inputs)
                    value = (value * 31 + ctx.attribute(input).toInt()) % 100003;
                return CalculationResult().setAttribute(output, value);
            };
            descriptors.append(d);
            QVERIFY(registry.registerCalculation(d));
        }
    }

    QList<DependencyKey> names;
    for (int n = 0; n < nameCount; ++n)
        names.append(DependencyKey::attribute(key(n)));

    FakeSessionState state;
    CalculationEngine engine(&state, &registry);

    for (int step = 0; step < 60; ++step) {
        const QByteArray where = QByteArray("seed ") + QByteArray::number(seed)
                               + " step " + QByteArray::number(step);
        const int op = pick(100);
        if (op < 70) {
            const DependencyKey name = names.at(pick(nameCount));
            QVERIFY2(engine.verifyAgainstFresh({name}).isEmpty(),
                     (where + " read of " + describe(name).toUtf8()).constData());
        } else if (op < 90) {
            const QString k = key(pick(nameCount));
            if (pick(2) == 0)
                state.removeAttribute(engine, k);
            else
                state.setAttribute(engine, k, pick(7));
        } else {
            const CalculationDescriptor &d = descriptors.at(pick(int(descriptors.size())));
            if (registry.contains(d.id))
                QVERIFY2(registry.unregister(d.id, CalculationRegistry::Removal::Change), where.constData());
            else
                QVERIFY2(registry.registerCalculation(d), where.constData());
        }
        QVERIFY2(engine.scopeDepth() == 0, where.constData());
    }

    QVERIFY2(engine.verifyAgainstFresh(names).isEmpty(), QByteArray::number(seed).constData());
    // And in the opposite order on a second engine: the same values.
    CalculationEngine other(&state, &registry);
    for (qsizetype i = names.size() - 1; i >= 0; --i) {
        const QString k = names.at(i).attributeKey;
        QVERIFY2(other.attribute(k) == engine.attribute(k), (QByteArray::number(seed) + ' ' + k.toLatin1()).constData());
    }
}

void CalcEngineOracleTest::randomizedExplicitSequences_data()
{
    QTest::addColumn<int>("seed");
    for (int seed = 1; seed <= 25; ++seed)
        QTest::addRow("seed %d", seed) << seed;
}

// Idempotency with explicit calculations in play (engine spec 7.4: an explicit
// output is a function of state once requested and unavailable before).
// Inspection never changes what a read returns, a refused or abandoned
// asynchronous request leaves no trace, and a published one is
// indistinguishable from a synchronous request.
void CalcEngineOracleTest::randomizedExplicitSequences()
{
    QFETCH(int, seed);
    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed));
    const auto pick = [&rng](int n) { return int(rng() % static_cast<unsigned>(n)); };
    using Prepare = CalculationEngine::PrepareOutcome;

    CalculationRegistry registry;
    FakePreferenceProvider prefs;
    registry.setPreferenceProvider(&prefs);
    Synthetic::registerSharedWorld(registry);
    Synthetic::registerExplicitWorld(registry);
    prefs.set("p", 5);

    Session s(registry);
    s.state.setAttribute("EA_IN", 4);
    s.state.setAttribute("EB_IN", 10);

    QList<DependencyKey> names = Synthetic::explicitNames();
    names << attr("X") << attr("Y") << attr("neg:EA1") << attr("neg:DB") << attr("EA_IN");
    const QStringList explicitIds = {"expA", "expB"};
    const QStringList inspectedIds = {"expA", "expB", "derivA", "derivA2", "derivB", "sum", "triple"};
    const QStringList editable = {"EA_IN", "EB_IN", "A"};
    const QStringList toggled = {"sum", "fallbackX", "neg", "altEaIn"};
    // On demand; no inputs; EA_IN = 3. Loses to a stored EA_IN, provides it otherwise.
    const auto altEaIn = []() {
        CalculationDescriptor d;
        d.id = QStringLiteral("altEaIn");
        d.outputs = {attr("EA_IN")};
        d.compute = [](const EvaluationContext &) { return CalculationResult().setAttribute("EA_IN", 3); };
        return d;
    };
    const auto explicitRuns = [&s]() { return s.engine.runCount("expA") + s.engine.runCount("expB"); };
    const auto edit = [&s, &pick](const QString &key) {
        if (pick(4) == 0)
            s.state.removeAttribute(s.engine, key);
        else
            s.state.setAttribute(s.engine, key, pick(9) - 2);   // negative EA_IN: expA rejects its input
    };

    // Literal checkpoint before anything random happens.
    QVERIFY(!s.engine.attribute("DDA").isValid());
    QCOMPARE(s.engine.blockers(attr("DB")).blockers.size(), 1);
    QCOMPARE(s.engine.blockers(attr("DB")).blockers.first().instanceId, QStringLiteral("expA"));
    QCOMPARE(explicitRuns(), 0);
    QCOMPARE(s.engine.request("expA").status, ResultStatus::Ok);
    QCOMPARE(s.engine.attribute("DDA"), QVariant(1105));
    QCOMPARE(s.engine.blockers(attr("DB")).blockers.first().instanceId, QStringLiteral("expB"));
    QCOMPARE(explicitRuns(), 1);
    QVERIFY(s.engine.verifyAgainstFresh(names).isEmpty());

    for (int step = 0; step < 300; ++step) {
        const int op = pick(100);
        const QByteArray where = QByteArray("seed ") + QByteArray::number(seed)
                               + " step " + QByteArray::number(step);
        const int runsBefore = explicitRuns();
        int runsAllowed = 0;

        if (op < 30) {
            const DependencyKey name = names.at(pick(int(names.size())));
            QVERIFY2(s.engine.verifyAgainstFresh({name}).isEmpty(),
                     (where + " read of " + describe(name).toUtf8()).constData());
        } else if (op < 46) {
            edit(editable.at(pick(int(editable.size()))));
        } else if (op < 50) {
            // Unregister or re-register (to the end) a candidate for a name
            // the explicit results look up, directly or through a passed-over
            // candidate: it loses, wins or is never tried depending on the
            // state, and the requested results must be dropped exactly when
            // their answers change.
            const QString id = toggled.at(pick(int(toggled.size())));
            if (registry.contains(id))
                QVERIFY2(registry.unregister(id, CalculationRegistry::Removal::Change), where.constData());
            else if (id == QLatin1String("sum"))
                QVERIFY2(registry.registerCalculation(Synthetic::sum()), where.constData());
            else if (id == QLatin1String("fallbackX"))
                QVERIFY2(registry.registerCalculation(Synthetic::fallbackX()), where.constData());
            else if (id == QLatin1String("neg"))
                QVERIFY2(registry.registerFamily(Synthetic::neg()), where.constData());
            else
                QVERIFY2(registry.registerCalculation(altEaIn()), where.constData());
        } else if (op < 72) {
            // Inspection, in any order and any number of times.
            for (int k = 0; k <= pick(3); ++k) {
                if (pick(2) == 0) {
                    const DependencyKey name = names.at(pick(int(names.size())));
                    const BlockerReport report = s.engine.blockers(name);
                    const bool available = s.engine.isAvailable(name);
                    QVERIFY2((report.state == BlockerReport::State::Available) == available,
                             (where + " blockers of " + describe(name).toUtf8()).constData());
                    QVERIFY2((report.state == BlockerReport::State::Blocked) == !report.blockers.isEmpty(),
                             where.constData());
                } else {
                    s.engine.readiness(inspectedIds.at(pick(int(inspectedIds.size()))));
                }
            }
        } else if (op < 80) {
            const CalculationEngine::RequestOutcome outcome =
                s.engine.request(explicitIds.at(pick(int(explicitIds.size()))));
            QVERIFY2(outcome.found, where.constData());
            runsAllowed = 1;
        } else if (op < 92) {
            // Asynchronous request, compute inline.
            const QString id = explicitIds.at(pick(int(explicitIds.size())));
            const CalculationReadiness before = s.engine.readiness(id);
            Prepare prepared = s.engine.prepare(id);
            QVERIFY2((prepared.kind == Prepare::Kind::Ready) == (before.state == CalculationReadiness::State::Ready),
                     where.constData());
            // (Not the converse: once "missing input" is cached for a blocked
            // calculation, prepare() reports that valid result instead.)
            QVERIFY2(prepared.kind != Prepare::Kind::Blocked || before.state == CalculationReadiness::State::Blocked,
                     where.constData());
            if (prepared.kind == Prepare::Kind::Ready) {
                // Still "not requested" while the ticket is outstanding.
                const DependencyKey name = names.at(pick(int(names.size())));
                QVERIFY2(s.engine.verifyAgainstFresh({name}).isEmpty(), where.constData());
                QVERIFY2(explicitRuns() == runsBefore, where.constData());
                if (pick(5) == 0) {
                    prepared.ticket.reset();    // abandoned
                } else {
                    const PublishOutcome outcome = prepared.ticket->publish(prepared.ticket->compute());
                    QVERIFY2(outcome.kind == PublishOutcome::Kind::Published, where.constData());
                    QVERIFY2(explicitRuns() == runsBefore + 1, where.constData());
                    runsAllowed = 1;
                }
            }
        } else {
            // Prepare, edit a declared input, publish: refused.
            const bool a = pick(2) == 0;
            Prepare prepared = s.engine.prepare(a ? "expA" : "expB");
            if (prepared.kind == Prepare::Kind::Ready) {
                ComputedCalculation computed = prepared.ticket->compute();
                edit(a ? QStringLiteral("EA_IN") : (pick(2) == 0 ? QStringLiteral("EA_IN") : QStringLiteral("EB_IN")));
                const PublishOutcome outcome = prepared.ticket->publish(std::move(computed));
                QVERIFY2(outcome.kind == PublishOutcome::Kind::RefusedStale, where.constData());
                QVERIFY2(outcome.reason == PublishOutcome::Reason::InputsChanged, where.constData());
                QVERIFY2(outcome.invalidated.isEmpty(), where.constData());
            }
        }

        // Explicit calculations run only in request / publish steps.
        const int ran = explicitRuns() - runsBefore;
        QVERIFY2(ran >= 0 && ran <= runsAllowed, where.constData());
        QVERIFY2(s.engine.scopeDepth() == 0, where.constData());
        QVERIFY2(s.engine.preparedCount() == 0, where.constData());

        for (int k = 0; k < 3; ++k) {
            const DependencyKey name = names.at(pick(int(names.size())));
            QVERIFY2(s.engine.verifyAgainstFresh({name}).isEmpty(),
                     (where + " read after step of " + describe(name).toUtf8()).constData());
        }
        QVERIFY2(explicitRuns() - runsBefore == ran, where.constData());
    }

    const QList<DependencyKey> mismatch = s.engine.verifyAgainstFresh(names);
    QVERIFY2(mismatch.isEmpty(),
             qPrintable(QStringLiteral("seed %1: %2 names differ from a fresh evaluation, first %3")
                            .arg(seed).arg(mismatch.size())
                            .arg(mismatch.isEmpty() ? QString() : describe(mismatch.first()))));
    QCOMPARE(s.engine.undeclaredReadCount(), 0);
    QCOMPARE(s.engine.cycleCount(), 0);
}

void CalcEngineOracleTest::registryChangePrecision_data()
{
    QTest::addColumn<QString>("row");
    QTest::addColumn<QStringList>("kept");      // cached before the change and after it
    QTest::addColumn<QStringList>("dropped");   // cached before the change, re-resolved after it

    const QStringList all = {"X", "Y", "Z", "W", "neg:A", "E", "C", "S/m", "S/d"};
    const auto allBut = [&all](const QStringList &out) {
        QStringList rest;
        for (const QString &n : all) {
            if (!out.contains(n))
                rest.append(n);
        }
        return rest;
    };

    QTest::newRow("candidate after the provider registered") << "afterProviderRegistered" << all << QStringList();
    QTest::newRow("candidate after the provider removed") << "afterProviderRemoved" << all << QStringList();
    QTest::newRow("provider removed") << "providerRemoved" << allBut({"X", "Y", "Z", "W", "S/d"})
                                      << QStringList({"X", "Y", "Z", "W", "S/d"});
    QTest::newRow("passed-over candidate removed") << "passedOverRemoved" << all << QStringList();
    QTest::newRow("passed-over family removed") << "passedOverFamilyRemoved" << all << QStringList();
    QTest::newRow("provider for a name resolved to nothing registered") << "nothingGetsProvider"
                                                                        << allBut({"E"}) << QStringList({"E"});
    QTest::newRow("family for a name resolved to nothing registered") << "nothingGetsFamily"
                                                                      << allBut({"neg:A"}) << QStringList({"neg:A"});
    QTest::newRow("stored value, candidate registered") << "storedCandidateRegistered" << all << QStringList();
    QTest::newRow("stored value, calculation that would provide it removed") << "storedProviderRemoved" << all
                                                                             << QStringList();
    QTest::newRow("recorded data, candidate registered") << "recordedCandidateRegistered" << all << QStringList();
    QTest::newRow("first conversion registered") << "firstConversion" << allBut({"S/m", "S/d"})
                                                 << QStringList({"S/m", "S/d"});
    QTest::newRow("conversion after the provider registered") << "conversionAfterProvider" << all << QStringList();
    QTest::newRow("last conversion removed") << "lastConversionRemoved" << allBut({"S/m", "S/d"})
                                             << QStringList({"S/m", "S/d"});
    QTest::newRow("passed-over conversion removed") << "passedOverConversionRemoved" << all << QStringList();
}

// A registry change re-resolves exactly the cached names whose answer it can
// alter (engine: registryChangeCanAlter): a candidate registered behind the
// provider or behind the session's own data, and a removed candidate that was
// passed over or never tried, leave the cached answer installed - served
// again without a run, and equal to a fresh evaluation; a candidate that can
// now answer a name that resolved to nothing, a removed provider, and a
// change of the conversion layer under recorded data drop it.
void CalcEngineOracleTest::registryChangePrecision()
{
    QFETCH(QString, row);
    QFETCH(QStringList, kept);
    QFETCH(QStringList, dropped);

    const auto key = [](const QString &name) {
        const qsizetype slash = name.indexOf(QLatin1Char('/'));
        return slash < 0 ? DependencyKey::attribute(name) : DependencyKey::measurement(name.left(slash), name.mid(slash + 1));
    };
    // A source conversion accepting every measurement: the samples and unit
    // passed through, or (provides = false) the same source leaves read and
    // nothing provided.
    const auto conversion = [](const QString &id, bool provides) {
        CalculationFamily f;
        f.id = id;
        f.instantiate = [provides](const DependencyKey &name) -> std::optional<CalculationDescriptor> {
            if (name.type != DependencyKey::Type::Measurement)
                return std::nullopt;
            const QString sensor = name.measurementKey.first;
            const QString meas = name.measurementKey.second;
            CalculationDescriptor d;
            d.id = sensor + QLatin1Char('/') + meas;
            d.inputs = {CalcInput::sourceMeasurement(sensor, meas), CalcInput::sourceUnit(sensor, meas)};
            d.outputs = {name};
            d.compute = [sensor, meas, provides](const EvaluationContext &ctx) {
                if (!provides)
                    return CalculationResult::unavailable();
                return CalculationResult().setMeasurement(sensor, meas, ctx.sourceMeasurement(sensor, meas),
                                                          ctx.sourceUnit(sensor, meas));
            };
            return d;
        };
        return f;
    };
    const auto constant = [](const QString &id, const DependencyKey &out) {
        CalculationDescriptor d;
        d.id = id;
        d.outputs = {out};
        d.compute = [out](const EvaluationContext &) {
            if (out.type == DependencyKey::Type::Measurement)
                return CalculationResult().setMeasurement(out.measurementKey.first, out.measurementKey.second,
                                                          {9.0}, QStringLiteral("calc"));
            return CalculationResult().setAttribute(out.attributeKey, 9);
        };
        return d;
    };

    CalculationRegistry registry;
    FakePreferenceProvider prefs;
    registry.setPreferenceProvider(&prefs);
    Synthetic::registerSharedWorld(registry);
    prefs.set("p", 5);
    Session s(registry);

    // The world before anything is read
    if (row == QLatin1String("passedOverRemoved") || row == QLatin1String("passedOverFamilyRemoved"))
        s.state.removeAttribute("A");     // sum and neg#neg:A miss A; constX provides X
    if (row.startsWith(QLatin1String("stored")))
        s.state.setAttribute("X", 8);
    if (row == QLatin1String("nothingGetsFamily"))
        QVERIFY(registry.unregister("neg", CalculationRegistry::Removal::Change));
    if (row == QLatin1String("conversionAfterProvider") || row == QLatin1String("lastConversionRemoved"))
        QVERIFY(registry.registerSourceConversion(conversion("conv", true)));
    if (row == QLatin1String("passedOverConversionRemoved")) {
        QVERIFY(registry.registerSourceConversion(conversion("convNone", false)));
        QVERIFY(registry.registerSourceConversion(conversion("conv", true)));
    }

    for (const QString &name : kept + dropped) {
        s.engine.isAvailable(key(name));
        QVERIFY2(s.engine.cachedState(key(name)) != CalculationEngine::CachedState::NotCached, qPrintable(name));
    }
    const int runs = s.engine.totalRunCount();

    if (row == QLatin1String("afterProviderRegistered") || row == QLatin1String("storedCandidateRegistered"))
        QVERIFY(registry.registerCalculation(constant("x9", attr("X"))));
    else if (row == QLatin1String("afterProviderRemoved"))
        QVERIFY(registry.unregister("fallbackX", CalculationRegistry::Removal::Change));
    else if (row == QLatin1String("providerRemoved") || row == QLatin1String("passedOverRemoved")
             || row == QLatin1String("storedProviderRemoved"))
        QVERIFY(registry.unregister("sum", CalculationRegistry::Removal::Change));
    else if (row == QLatin1String("passedOverFamilyRemoved"))
        QVERIFY(registry.unregister("neg", CalculationRegistry::Removal::Change));
    else if (row == QLatin1String("nothingGetsProvider"))
        QVERIFY(registry.registerCalculation(constant("constE", attr("E"))));
    else if (row == QLatin1String("nothingGetsFamily"))
        QVERIFY(registry.registerFamily(Synthetic::neg()));
    else if (row == QLatin1String("recordedCandidateRegistered"))
        QVERIFY(registry.registerCalculation(constant("smCalc", measKey("S", "m"))));
    else if (row == QLatin1String("firstConversion") || row == QLatin1String("conversionAfterProvider"))
        QVERIFY(registry.registerSourceConversion(conversion(row == QLatin1String("firstConversion")
                                                                 ? QStringLiteral("conv") : QStringLiteral("conv2"),
                                                             true)));
    else if (row == QLatin1String("lastConversionRemoved"))
        QVERIFY(registry.unregister("conv", CalculationRegistry::Removal::Change));
    else if (row == QLatin1String("passedOverConversionRemoved"))
        QVERIFY(registry.unregister("convNone", CalculationRegistry::Removal::Change));
    else
        QFAIL("unknown row");
    QCOMPARE(s.engine.totalRunCount(), runs);

    for (const QString &name : std::as_const(kept))
        QVERIFY2(s.engine.cachedState(key(name)) != CalculationEngine::CachedState::NotCached, qPrintable(name));
    for (const QString &name : std::as_const(dropped))
        QVERIFY2(s.engine.cachedState(key(name)) == CalculationEngine::CachedState::NotCached, qPrintable(name));

    // A kept answer is served as it is - nothing runs - and equals a fresh evaluation
    for (const QString &name : std::as_const(kept))
        QVERIFY2(s.engine.verifyAgainstFresh({key(name)}).isEmpty(), qPrintable(name));
    QCOMPARE(s.engine.totalRunCount(), runs);
    QList<DependencyKey> all;
    for (const QString &name : kept + dropped)
        all.append(key(name));
    QVERIFY(s.engine.verifyAgainstFresh(all).isEmpty());
    QCOMPARE(s.engine.cycleCount(), 0);
}

FLYSIGHT_TEST_MAIN(CalcEngineOracleTest)
#include "tst_calcengine_oracle.moc"
