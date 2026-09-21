// The system-time-to-UTC fit (builtin.time.fit) on the calculation engine,
// against a fake session state and a private registry: precision at high
// device uptime, invalidation through the TIME sensor, GPS week rollover, and
// degenerate clocks. Every expectation is a literal or the literal clock model
// of the test itself; nothing is obtained from the code under test.

#include <algorithm>
#include <cmath>
#include <memory>

#include <QtTest>

#include "calculations/builtincalculations.h"
#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "fakesessionstate.h"
#include "preferences/preferencekeys.h"
#include "testmain.h"

using namespace FlySight;
using namespace FlySightTest;
using Synthetic::attr;
using Synthetic::measKey;

namespace {

// A private registry with every built-in, synthetic preferences, a fake
// session state, and an engine over them. Members are destroyed in reverse
// order, so the engine goes before the registry.
struct World {
    CalculationRegistry registry;
    FakePreferenceProvider prefs;
    FakeSessionState state;
    std::unique_ptr<CalculationEngine> engine;

    World()
    {
        registerBuiltInCalculations(registry);
        prefs.set(PreferenceKeys::ImportDescentPauseSeconds, 30.0);
        registry.setPreferenceProvider(&prefs);
        engine = std::make_unique<CalculationEngine>(&state, &registry);
    }
};

// Time pulses as a FlySight records them: system time next to GPS week and
// time of week (file-format definition: UTC = week * 604800 + tow + 315964800).
// The IMU runs on the system clock and the GNSS on UTC, at the same instants.
void addClock(FakeSessionState &state, const QVector<double> &system, const QVector<double> &utc)
{
    QVector<double> week, tow;
    for (double value : utc) {
        const double w = std::floor((value - 315964800.0) / 604800.0);
        week.append(w);
        tow.append(value - 315964800.0 - w * 604800.0);
    }
    state.setMeasurement("TIME", "time", system);
    state.setMeasurement("TIME", "tow", tow);
    state.setMeasurement("TIME", "week", week);
    state.setMeasurement("IMU", "time", system);
    state.setMeasurement("GNSS", "time", utc);
}

// An exactly linear clock after almost three days of uptime.
constexpr double Epoch = 1725729984.6;
constexpr double Slope = 1.0000053;
constexpr int PulseCount = 1850;

void addHighUptimeClock(FakeSessionState &state, QVector<double> *system, QVector<double> *utc)
{
    for (int i = 0; i < PulseCount; ++i) {
        system->append(247616.413 + i / Slope);
        utc->append(Epoch + i);
    }
    addClock(state, *system, *utc);
}

double maxAbsDifference(const QVector<double> &a, const QVector<double> &b, double offset = 0.0)
{
    double maximum = 0.0;
    for (int i = 0; i < a.size(); ++i)
        maximum = std::max(maximum, std::abs(a[i] - b[i] - offset));
    return maximum;
}

} // namespace

class TimeFitTest : public QObject {
    Q_OBJECT

private slots:
    void highUptimeExactClock();
    void fitFollowsTimeSource();
    void weekRollover();
    void degenerateFits_data();
    void degenerateFits();
    void fixtureFitIsExact();
};

// The regression test of the centered sums: with the uncentered normal
// equations the first bound fails by four orders of magnitude (tens of
// milliseconds), because N * sumSU and sumS * sumU are products of Unix UTC
// and device uptime that agree in almost all the digits a double has.
void TimeFitTest::highUptimeExactClock()
{
    World world;
    QVector<double> system, utc;
    addHighUptimeClock(world.state, &system, &utc);
    CalculationEngine &engine = *world.engine;

    const QVector<double> converted = engine.measurement("IMU", "_time");
    QCOMPARE(converted.size(), 1850);
    const double utcError = maxAbsDifference(converted, utc);
    QVERIFY2(utcError < 2e-6, qPrintable(QString::number(utcError, 'g', 17)));

    QVERIFY(std::abs(engine.attribute("_TIME_FIT_A").toDouble() - 1.0000053) < 1e-9);

    const QVector<double> back = engine.measurement("GNSS", "_system_time");
    QCOMPARE(back.size(), 1850);
    const double systemError = maxAbsDifference(back, system);
    QVERIFY2(systemError < 2e-6, qPrintable(QString::number(systemError, 'g', 17)));

    QCOMPARE(engine.runCount("builtin.time.fit"), 1);
    QCOMPARE(engine.undeclaredReadCount(), 0);
}

// A change to the TIME sensor takes both coefficients and every converted time
// with it, and the new fit is as precise as the first.
void TimeFitTest::fitFollowsTimeSource()
{
    World world;
    QVector<double> system, utc;
    addHighUptimeClock(world.state, &system, &utc);
    CalculationEngine &engine = *world.engine;

    const QVector<double> converted = engine.measurement("IMU", "_time");
    QCOMPARE(converted.size(), 1850);
    QVERIFY(engine.attribute("_TIME_FIT_B").isValid());
    QCOMPARE(engine.runCount("builtin.time.fit"), 1);

    QVector<double> tow = world.state.sourceMeasurement("TIME", "tow");
    QCOMPARE(tow.size(), 1850);
    for (double &value : tow)
        value += 2.0;
    const QSet<DependencyKey> invalidated = world.state.setMeasurement(engine, "TIME", "tow", tow);
    QVERIFY(invalidated.contains(attr("_TIME_FIT_A")));
    QVERIFY(invalidated.contains(attr("_TIME_FIT_B")));
    QVERIFY(invalidated.contains(measKey("IMU", "_time")));

    const QVector<double> shifted = engine.measurement("IMU", "_time");
    QCOMPARE(shifted.size(), 1850);
    const double error = maxAbsDifference(shifted, converted, 2.0);
    QVERIFY2(error < 2e-6, qPrintable(QString::number(error, 'g', 17)));

    QCOMPARE(engine.runCount("builtin.time.fit"), 2);
    QCOMPARE(engine.undeclaredReadCount(), 0);
}

// UTC is continuous across a GPS week boundary. The values are small integers
// around the means, so the centered fit is exact.
void TimeFitTest::weekRollover()
{
    World world;
    const double boundary = 315964800.0 + 2400 * 604800.0;
    addClock(world.state, {100, 101, 102}, {boundary - 1, boundary, boundary + 1});
    CalculationEngine &engine = *world.engine;

    QCOMPARE(world.state.sourceMeasurement("TIME", "week"), QVector<double>({2399, 2400, 2400}));
    QCOMPARE(world.state.sourceMeasurement("TIME", "tow"), QVector<double>({604799, 0, 1}));

    const QVector<double> converted = engine.measurement("IMU", "_time");
    QCOMPARE(converted, engine.measurement("GNSS", "time"));
    QVERIFY(converted == QVector<double>({1767484799.0, 1767484800.0, 1767484801.0}));
    QCOMPARE(engine.undeclaredReadCount(), 0);
}

void TimeFitTest::degenerateFits_data()
{
    QTest::addColumn<QVector<double>>("system");
    QTest::addColumn<QVector<double>>("utc");
    QTest::addColumn<bool>("hasTimeSensor");
    QTest::addColumn<int>("runs");

    QTest::newRow("one pulse") << QVector<double>({1}) << QVector<double>({1725729984.6}) << true << 1;
    QTest::newRow("equal system times") << QVector<double>({1, 1})
                                        << QVector<double>({1725729984.6, 1725729985.6}) << true << 1;
    QTest::newRow("no TIME sensor") << QVector<double>({1, 2})
                                    << QVector<double>({1725729984.6, 1725729985.6}) << false << 0;
}

// A clock that cannot be fitted produces nothing: the fit ran and both
// coefficients are unavailable. Without a TIME sensor it does not run at all.
void TimeFitTest::degenerateFits()
{
    QFETCH(QVector<double>, system);
    QFETCH(QVector<double>, utc);
    QFETCH(bool, hasTimeSensor);
    QFETCH(int, runs);

    World world;
    if (hasTimeSensor) {
        addClock(world.state, system, utc);
    } else {
        world.state.setMeasurement("IMU", "time", system);
        world.state.setMeasurement("GNSS", "time", utc);
    }
    CalculationEngine &engine = *world.engine;

    QVERIFY(!engine.attribute("_TIME_FIT_A").isValid());
    QVERIFY(!engine.attribute("_TIME_FIT_B").isValid());
    QVERIFY(engine.measurement("IMU", "_time").isEmpty());

    QVERIFY(engine.resultStatus("builtin.time.fit")
            == (hasTimeSensor ? ResultStatus::Ok : ResultStatus::MissingInput));
    QCOMPARE(engine.runCount("builtin.time.fit"), runs);
    QCOMPARE(engine.undeclaredReadCount(), 0);
}

// The clock of the descent fixture, stated here so that a failure points at
// the fit rather than at one of its consumers.
void TimeFitTest::fixtureFitIsExact()
{
    World world;
    world.state.setMeasurement("TIME", "time", {10, 20, 30});
    world.state.setMeasurement("TIME", "tow", {129610, 129620, 129630});
    world.state.setMeasurement("TIME", "week", {2295, 2295, 2295});
    CalculationEngine &engine = *world.engine;

    const QVariant a = engine.attribute("_TIME_FIT_A");
    const QVariant b = engine.attribute("_TIME_FIT_B");
    QCOMPARE(a.userType(), int(QMetaType::QString));
    QCOMPARE(b.userType(), int(QMetaType::QString));
    QCOMPARE(a.toString(), QStringLiteral("1"));
    QCOMPARE(b.toString(), QStringLiteral("1704110400"));

    QCOMPARE(engine.runCount("builtin.time.fit"), 1);
    QCOMPARE(engine.undeclaredReadCount(), 0);
}

FLYSIGHT_TEST_MAIN(TimeFitTest)
#include "tst_time_fit.moc"
