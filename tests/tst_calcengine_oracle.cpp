// Idempotency oracle (spec 7.4, acceptance 10): after any sequence of reads,
// edits, preference changes, and registry changes, the value returned for every
// name equals the value obtained from a fresh evaluation with empty caches.
//
// Randomized but reproducible: std::mt19937 with a literal seed, reduced with
// `rng() % n` only (std::uniform_int_distribution differs between standard
// libraries). This is the one place where a computed value is the expectation,
// as the spec mandates; every sequence also starts with literal checkpoints.

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

QList<DependencyKey> allNames()
{
    return {attr("X"), attr("Y"), attr("Z"), attr("W"), attr("X2"), attr("Y2"),
            attr("neg:A"), attr("neg:X"), measKey("S", "d"), measKey("S", "m")};
}

// The cycle in the shared world (P/R) is reported every time X2 / Y2
// re-resolve; thousands of identical warnings would bury a real failure.
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

// Acceptance 10 (and spec 7.4): read order and cache contents never change the
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
    prefs.set("p", 5);

    Session first(registry), second(registry);
    Session *const sessions[2] = {&first, &second};
    const QList<DependencyKey> names = allNames();

    // Literal checkpoint before anything random happens.
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

    const QtMessageHandler previousHandler = qInstallMessageHandler(quietHandler);
    struct RestoreHandler {
        QtMessageHandler handler;
        ~RestoreHandler() { qInstallMessageHandler(handler); }
    } restore{previousHandler};

    const QStringList editable = {"A", "B", "C", "X", "Z", "Y2"};
    const QStringList toggled = {"fallbackX", "wAlt", "neg"};

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
            } else {
                QVERIFY2(registry.registerFamily(Synthetic::neg()), where.constData());
            }
        }

        // No notification ever computes, in either session.
        QVERIFY2(sessions[0]->engine.totalRunCount() == runs0, where.constData());
        QVERIFY2(sessions[1]->engine.totalRunCount() == runs1, where.constData());
        QVERIFY2(s.engine.scopeDepth() == 0, where.constData());
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

FLYSIGHT_TEST_MAIN(CalcEngineOracleTest)
#include "tst_calcengine_oracle.moc"
