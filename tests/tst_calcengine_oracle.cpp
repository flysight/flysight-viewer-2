// Idempotency oracle (acceptance 10): after any sequence of reads,
// edits, preference changes, and registry changes, the value returned for every
// name equals the value obtained from a fresh evaluation with empty caches.
//
// Randomized but reproducible: std::mt19937 with a literal seed, reduced with
// `rng() % n` only (std::uniform_int_distribution differs between standard
// libraries). This is the one place where a computed value is the expectation,
// because the rule under test IS "equal to a fresh evaluation"; every sequence
// also starts with literal checkpoints.

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
                QVERIFY2(registry.unregister(id), where.constData());
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
                QVERIFY2(registry.unregister(d.id), where.constData());
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

FLYSIGHT_TEST_MAIN(CalcEngineOracleTest)
#include "tst_calcengine_oracle.moc"
