// The built-in calculations on the calculation engine, against a fake session
// state and a private registry: golden values, the registration inventory,
// declared inputs only, multi-output groups, ordered candidates, the declared
// preference input, the interpolation family, and the altitude descriptor.

#include <cmath>
#include <limits>
#include <memory>
#include <utility>

#include <QCryptographicHash>
#include <QScopeGuard>
#include <QtTest>

#include "altitudemarkerfeature.h"
#include "builtinfixture.h"
#include "calculations/builtincalculations.h"
#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "calculations/interpolationcalculations.h"
#include "fakesessionstate.h"
#include "logbookprobe.h"
#include "preferences/preferencesmanager.h"
#include "preferences/preferencekeys.h"
#include "sessiondata.h"
#include "testenvironment.h"
#include "testmain.h"

using namespace FlySight;
using namespace FlySightTest;
using Synthetic::attr;
using Synthetic::measKey;

namespace {

constexpr double T0 = DescentFixture::T0;

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

void addTimeData(FakeSessionState &state)
{
    state.setMeasurement("TIME", "time", {10, 20, 30});
    state.setMeasurement("TIME", "tow", {129610, 129620, 129630});
    state.setMeasurement("TIME", "week", {2295, 2295, 2295});
}

// The eight-sample descent used by the preference tests.
void addPausedDescent(FakeSessionState &state)
{
    state.setMeasurement("GNSS", "time", {0, 10, 20, 30, 40, 50, 60, 70});
    state.setMeasurement("GNSS", "hMSL", {1000, 900, 800, 810, 820, 830, 700, 600});
}

} // namespace

class BuiltinsEngineTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void inventory();
    void goldenOnEngine_data();
    void goldenOnEngine();
    void noUndeclaredReads();
    void constantDefaults();

    void timeFitRunsOnce();

    void accelerationAccuracyKnownAnswers();
    void accelerationAccuracyUnavailable();
    void accelerationAcrossHole();

    void analysisRangeFollowsPreference();
    void analysisRangeNeedsPreference();
    void startTimeCandidateOrder();

    void wspPartial();
    void spWindowStartIsOneRun();
    void simplifiedRunsOnce();

    void interpolationInstances();
    void interpolationUnavailableIsCached();
    void interpolationInsideHole();
    void groundElevationInsideHole();
    void exitCrossingBesideHole();
    void altitudeCrossingAcrossHole();
    void altitudeDescriptor();

    // Logbook column cache: static closures and the column environment digest
    void gyroColumnClosure();
    void digestChanges();
    void digestCoversResultVersions();
    void digestCoversConversionLayer();
    void digestSurvivesRuntimeAltitudeMarker();
    void altitudeMarkerTeardownReportsNothing();

private:
    SessionData m_fixture;              // imported once; copied into each fake
    std::unique_ptr<World> m_world;     // descent fixture, fresh per test function
};

void BuiltinsEngineTest::initTestCase()
{
    // The importer reads preferences and the attribute registry; the
    // calculations under test are registered on private registries only.
    TestEnvironment::instance().resetPreferencesToDefaults();
    m_fixture = DescentFixture::load();
}

void BuiltinsEngineTest::init()
{
    m_world = std::make_unique<World>();
    copyStoredState(m_fixture, m_world->state);
}

void BuiltinsEngineTest::cleanup()
{
    m_world.reset();
}

void BuiltinsEngineTest::inventory()
{
    const QStringList expected = {
        // the conversion layer (source conversions, in candidate order)
        "builtin.conversion.schema",
        "builtin.conversion.default",
        // attributecalculations
        "builtin.attr.analysisRange",
        "builtin.attr.exitTime",
        "builtin.attr.syncTime",
        "builtin.attr.courseRef",
        "builtin.default._WIND_N",
        "builtin.default._WIND_E",
        "builtin.default._COMPUTE",
        "builtin.attr.manoeuvreStart",
        "builtin.attr.flare",
        "builtin.attr.landingTime",
        "builtin.attr.timeExtent.GNSS",
        "builtin.attr.timeExtent.BARO",
        "builtin.attr.timeExtent.HUM",
        "builtin.attr.timeExtent.MAG",
        "builtin.attr.timeExtent.IMU",
        "builtin.attr.timeExtent.TIME",
        "builtin.attr.timeExtent.VBAT",
        "builtin.attr.maxVelDTime",
        "builtin.attr.maxVelHTime",
        "builtin.attr.groundElev",
        // gnsscalculations
        "builtin.gnss.z",
        "builtin.gnss.velH",
        "builtin.gnss.vel",
        "builtin.gnss.accD",
        "builtin.gnss.accN",
        "builtin.gnss.accE",
        "builtin.gnss.accAcc",
        "builtin.gnss.wcVel",
        "builtin.gnss.course",
        "builtin.gnss.courseRate",
        "builtin.gnss.glideRatio",
        "builtin.gnss.diveAngle",
        "builtin.gnss.diveAngleRate",
        "builtin.gnss.accH",
        "builtin.gnss.wcVelH",
        "builtin.gnss.accAlongTrack",
        "builtin.gnss.accCrossTrack",
        "builtin.gnss.lift",
        "builtin.gnss.drag",
        "builtin.gnss.specificEnergy",
        "builtin.gnss.specificEnergyRate",
        // imucalculations, magcalculations
        "builtin.imu.aTotal",
        "builtin.imu.wTotal",
        "builtin.mag.total",
        // timecalculations
        "builtin.time.fit",
        "builtin.time.utc.GNSS",
        "builtin.time.utc.BARO",
        "builtin.time.utc.HUM",
        "builtin.time.utc.MAG",
        "builtin.time.utc.IMU",
        "builtin.time.utc.TIME",
        "builtin.time.utc.VBAT",
        "builtin.time.system.GNSS",
        "builtin.time.system.BARO",
        "builtin.time.system.HUM",
        "builtin.time.system.MAG",
        "builtin.time.system.IMU",
        "builtin.time.system.TIME",
        "builtin.time.system.VBAT",
        // localcoordinatecalculations
        "builtin.local.coordinates",
        "builtin.local.time",
        "builtin.local.systemTime",
        // simplificationcalculations
        "builtin.simplified.track",
        // wspcalculations
        "builtin.default._WSP_VERSION",
        "builtin.default._WSP_TOP_ALT",
        "builtin.default._WSP_BOTTOM_ALT",
        "builtin.default._WSP_TASK",
        "builtin.wsp.ref1Time",
        "builtin.wsp.results",
        // spcalculations
        "builtin.default._SP_PERF_WINDOW_HEIGHT",
        "builtin.default._SP_VAL_WINDOW_HEIGHT",
        "builtin.default._SP_BREAKOFF_ALT",
        "builtin.sp.windowStart",
        "builtin.sp.results",
        // synthesized interpolation, last
        "builtin.interpolation",
    };

    const QStringList ids = m_world->registry.registeredIds();
    QCOMPARE(ids, expected);
    QCOMPARE(ids.size(), 77);   // 2 conversion families + 74 calculations + 1 family

    const QStringList families = {"builtin.conversion.schema", "builtin.conversion.default",
                                  "builtin.interpolation"};
    for (const QString &id : ids)
        QCOMPARE(m_world->registry.isFamily(id), families.contains(id));
    QVERIFY(m_world->registry.hasSourceConversions());

    // Registering the built-ins touches no process-wide registry, so it can be
    // repeated on any number of private registries.
    CalculationRegistry second;
    registerBuiltInCalculations(second);
    QCOMPARE(second.registeredIds(), expected);
    QVERIFY(CalculationRegistry::instance().registeredIds().isEmpty());
}

void BuiltinsEngineTest::goldenOnEngine_data()
{
    QTest::addColumn<int>("row");
    const QList<GoldenValue> golden = goldenValues();
    for (int i = 0; i < golden.size(); ++i)
        QTest::newRow(qPrintable(goldenTag(golden.at(i).name))) << i;
}

void BuiltinsEngineTest::goldenOnEngine()
{
    QFETCH(int, row);
    const GoldenValue golden = goldenValues().at(row);
    CalculationEngine &engine = *m_world->engine;

    // Every available golden value is a calculation output, not stored data.
    if (golden.available)
        QVERIFY(m_world->registry.hasCandidateFor(golden.name));

    QVariant attribute;
    QVector<double> samples;
    if (golden.name.type == DependencyKey::Type::Attribute)
        attribute = engine.attribute(golden.name.attributeKey);
    else
        samples = engine.measurement(golden.name.measurementKey.first,
                                     golden.name.measurementKey.second);

    const QString difference = compareToGolden(golden, attribute, samples);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));

    QCOMPARE(engine.undeclaredReadCount(), 0);
    QCOMPARE(engine.cycleCount(), 0);
}

// Every compute function reads only what it declared, and the declared-input
// graph of the built-ins is acyclic (nothing relies on cycle fallback).
void BuiltinsEngineTest::noUndeclaredReads()
{
    CalculationEngine &engine = *m_world->engine;

    int plain = 0;
    for (const QString &id : m_world->registry.registeredIds()) {
        if (m_world->registry.isFamily(id))
            continue;
        ++plain;
        const CalculationEngine::RequestOutcome outcome = engine.request(id);
        QVERIFY2(outcome.found, qPrintable(id));

        const std::optional<ResultStatus> status = engine.resultStatus(id);
        QVERIFY2(status.has_value(), qPrintable(id));
        QVERIFY2(*status == ResultStatus::Ok || *status == ResultStatus::MissingInput, qPrintable(id));
    }
    QCOMPARE(plain, 74);

    QCOMPARE(engine.undeclaredReadCount(), 0);
    QCOMPARE(engine.cycleCount(), 0);
    QCOMPARE(engine.scopeDepth(), 0);

    // On the full fixture every built-in has its inputs.
    for (const QString &id : m_world->registry.registeredIds()) {
        if (!m_world->registry.isFamily(id))
            QVERIFY2(engine.resultStatus(id) == ResultStatus::Ok, qPrintable(id));
    }
}

// Every constant default is registered by Calculations::addConstantDefault:
// the id builtin.default.<key>, no inputs, the attribute as its one output.
// The engine's proof that such a calculation serves as a default is
// tst_calcengine::constantCalculationIsADefault; this pins the built-in ones.
void BuiltinsEngineTest::constantDefaults()
{
    const QString prefix = QStringLiteral("builtin.default.");
    const QHash<QString, QVariant> expected = {
        {QStringLiteral("_WIND_N"), QVariant(0.0)},
        {QStringLiteral("_WIND_E"), QVariant(0.0)},
        {QStringLiteral("_COMPUTE"), QVariant(QStringLiteral("on"))},
        {QStringLiteral("_WSP_VERSION"), QVariant(QStringLiteral("1.0"))},
        {QStringLiteral("_WSP_TOP_ALT"), QVariant(2500.0)},
        {QStringLiteral("_WSP_BOTTOM_ALT"), QVariant(1500.0)},
        {QStringLiteral("_WSP_TASK"), QVariant(QStringLiteral("Time"))},
        {QStringLiteral("_SP_PERF_WINDOW_HEIGHT"), QVariant(7400.0 / 3.28084)},
        {QStringLiteral("_SP_VAL_WINDOW_HEIGHT"), QVariant(3300.0 / 3.28084)},
        {QStringLiteral("_SP_BREAKOFF_ALT"), QVariant(5600.0 / 3.28084)},
    };

    // The shape of every registration under the prefix, and exactly these ten.
    QSet<QString> found;
    for (const QString &id : m_world->registry.registeredIds()) {
        if (!id.startsWith(prefix))
            continue;
        const QString key = id.mid(prefix.size());
        found.insert(key);
        const std::optional<CalculationInstance> instance = m_world->registry.instance(id);
        QVERIFY2(instance.has_value(), qPrintable(id));
        QVERIFY2(instance->descriptor->inputs.isEmpty(), qPrintable(id));
        QCOMPARE(instance->descriptor->outputs.size(), 1);
        QVERIFY2(instance->descriptor->outputs.first() == DependencyKey::attribute(key), qPrintable(id));
    }
    QCOMPARE(found, QSet<QString>(expected.keyBegin(), expected.keyEnd()));

    // Their values and value types, on a session that stores none of them.
    World empty;
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        const QVariant value = empty.engine->attribute(it.key());
        QCOMPARE(value.userType(), it.value().userType());
        QCOMPARE(value, it.value());
        QVERIFY(empty.engine->resultStatus(prefix + it.key()) == ResultStatus::Ok);
    }

    // The descent fixture is imported and stores no wind: wind reads zero from
    // its default. A stored value wins and moves the wind-corrected speed;
    // removing it returns both to the default.
    QVERIFY(!m_fixture.hasStoredAttribute(SessionKeys::WindN));
    QVERIFY(!m_fixture.hasStoredAttribute(SessionKeys::WindE));
    CalculationEngine &engine = *m_world->engine;
    QCOMPARE(engine.attribute("_WIND_N"), QVariant(0.0));
    const QVector<double> calm = engine.measurement("GNSS", "wcVel");
    QVERIFY(!calm.isEmpty());

    QVERIFY(m_world->state.setAttribute(engine, "_WIND_N", 5.0).contains(measKey("GNSS", "wcVel")));
    QCOMPARE(engine.attribute("_WIND_N"), QVariant(5.0));
    const QVector<double> windy = engine.measurement("GNSS", "wcVel");
    QCOMPARE(windy.size(), calm.size());
    QVERIFY(windy != calm);

    QVERIFY(m_world->state.removeAttribute(engine, "_WIND_N").contains(measKey("GNSS", "wcVel")));
    QCOMPARE(engine.attribute("_WIND_N"), QVariant(0.0));
    QCOMPARE(engine.measurement("GNSS", "wcVel"), calm);
    QCOMPARE(engine.undeclaredReadCount(), 0);
}

void BuiltinsEngineTest::timeFitRunsOnce()
{
    CalculationEngine &engine = *m_world->engine;

    const QVariant b = engine.attribute("_TIME_FIT_B");
    const QVariant a = engine.attribute("_TIME_FIT_A");
    QCOMPARE(a.userType(), int(QMetaType::QString));
    QCOMPARE(a.toString(), QStringLiteral("1"));
    QCOMPARE(b.toString(), QStringLiteral("1704110400"));

    QCOMPARE(engine.measurement("IMU", "_time"), QVector<double>({T0 + 10.0, T0 + 20.0, T0 + 30.0}));
    QCOMPARE(engine.measurement("MAG", "_time"), QVector<double>({T0 + 10.0, T0 + 20.0, T0 + 30.0}));

    QCOMPARE(engine.runCount("builtin.time.fit"), 1);
}

// GNSS/accAcc is sqrt(sAcc[i+1]^2 + sAcc[i-1]^2) / (t[i+1] - t[i-1]) inside
// and the one-interval form at each end, bit for bit. The speed accuracies
// are dyadic with short mantissas, so every square and every sum of two is
// exact and a compiler that fuses the sum into a multiply-add, here or in the
// calculation, cannot move a bit; the times are a quarter second apart (on
// the epoch's scale a quarter and a sixteenth are exact) with one late fix,
// 5/16 s after the one before it, so the interior steps are not all equal.
// That step is 1.25 nominal intervals, clearly not a hole (more than 1.5, the
// continuity rule): the stencil divides by the actual intervals, and a hole
// is accelerationAcrossHole()'s case.
void BuiltinsEngineTest::accelerationAccuracyKnownAnswers()
{
    World world;
    const QVector<double> t = {T0, T0 + 0.25, T0 + 0.5, T0 + 0.75, T0 + 1.0625, T0 + 1.3125};
    const QVector<double> sAcc = {0.75, 1.0, 1.0, 0.5, 2.25, 3.0};
    world.state.setMeasurement("GNSS", "time", t);
    world.state.setMeasurement("GNSS", "sAcc", sAcc);
    CalculationEngine &engine = *world.engine;

    const QVector<double> accAcc = engine.measurement("GNSS", "accAcc");
    QCOMPARE(accAcc.size(), engine.measurement("GNSS", "_time").size());
    QCOMPARE(accAcc.size(), t.size());

    const int last = t.size() - 1;
    QVector<double> expected;
    expected.append(std::sqrt(sAcc[1] * sAcc[1] + sAcc[0] * sAcc[0]) / (t[1] - t[0]));
    for (int i = 1; i < last; ++i)
        expected.append(std::sqrt(sAcc[i + 1] * sAcc[i + 1] + sAcc[i - 1] * sAcc[i - 1]) / (t[i + 1] - t[i - 1]));
    expected.append(std::sqrt(sAcc[last] * sAcc[last] + sAcc[last - 1] * sAcc[last - 1]) / (t[last] - t[last - 1]));
    for (int i = 0; i < t.size(); ++i) {
        QVERIFY2(accAcc[i] == expected[i],
                 qPrintable(QStringLiteral("accAcc[%1] = %2, the formula %3")
                                .arg(i).arg(accAcc[i], 0, 'g', 17).arg(expected[i], 0, 'g', 17)));
    }

    // Three that are exact by hand: each end divides by its one interval,
    // the first interior sample by two
    QVERIFY(accAcc[0] == 5.0);      // sqrt(1.0^2 + 0.75^2) / 0.25
    QVERIFY(accAcc[1] == 2.5);      // sqrt(1.0^2 + 0.75^2) / 0.5
    QVERIFY(accAcc[last] == 15.0);  // sqrt(3.0^2 + 2.25^2) / 0.25

    QCOMPARE(engine.undeclaredReadCount(), 0);
}

// Unavailable, as the derivative is: without the speed accuracy or the time,
// with one sample, and with lengths that differ
void BuiltinsEngineTest::accelerationAccuracyUnavailable()
{
    const DependencyKey accAcc = measKey("GNSS", "accAcc");
    const auto unavailable = [&accAcc](const QVector<double> &time, const QVector<double> &sAcc) {
        World world;
        if (!time.isEmpty())
            world.state.setMeasurement("GNSS", "time", time);
        if (!sAcc.isEmpty())
            world.state.setMeasurement("GNSS", "sAcc", sAcc);
        CalculationEngine &engine = *world.engine;
        return engine.measurement("GNSS", "accAcc").isEmpty() && !engine.isAvailable(accAcc)
            && engine.undeclaredReadCount() == 0;
    };

    QVERIFY(unavailable({T0, T0 + 1.0, T0 + 2.0}, {}));
    QVERIFY(unavailable({}, {0.5, 0.5, 0.5}));
    QVERIFY(unavailable({T0}, {0.5}));
    QVERIFY(unavailable({T0, T0 + 1.0, T0 + 2.0}, {0.5, 0.5}));
    QVERIFY(unavailable({T0, T0 + 1.0}, {0.5, 0.5, 0.5}));
    // and the same world with two samples has it
    QVERIFY(!unavailable({T0, T0 + 1.0}, {0.5, 0.5}));
}

// The derivative and its accuracy on the known-answer data with a hole cut
// in (an interval of three quarter seconds among quarter seconds, above 1.5
// nominal intervals): NaN exactly at the samples whose stencil holds the
// hole, the ends' one-interval forms included, and at every other sample the
// formula on the same data, bit for bit. The hole is an interior interval,
// the first and the last. accN, accE and accD difference GNSS/time and
// accAcc GNSS/_time, the same buffer, so all four are NaN at the same
// samples.
void BuiltinsEngineTest::accelerationAcrossHole()
{
    struct Case {
        const char *name;
        QVector<double> t;
        QList<int> unavailable;
    };
    const Case cases[] = {
        {"interior", {T0, T0 + 0.25, T0 + 0.5, T0 + 0.75, T0 + 1.5, T0 + 1.75, T0 + 2.0}, {3, 4}},
        {"first", {T0, T0 + 0.75, T0 + 1.0, T0 + 1.25, T0 + 1.5, T0 + 1.75, T0 + 2.0}, {0, 1}},
        {"last", {T0, T0 + 0.25, T0 + 0.5, T0 + 0.75, T0 + 1.0, T0 + 1.25, T0 + 2.0}, {5, 6}},
    };
    const QVector<double> sAcc = {0.75, 1.0, 1.0, 0.5, 2.25, 3.0, 0.5};
    const QVector<double> velN = {10.0, 12.5, 11.0, 9.5, 20.0, 18.25, 17.0};
    const QVector<double> velE = {-1.0, 0.5, 2.0, 2.5, -3.0, -3.5, 0.25};
    const QVector<double> velD = {50.0, 49.5, 48.0, 51.0, 45.5, 44.0, 43.75};

    for (const Case &c : cases) {
        World world;
        world.state.setMeasurement("GNSS", "time", c.t);
        world.state.setMeasurement("GNSS", "sAcc", sAcc);
        world.state.setMeasurement("GNSS", "velN", velN);
        world.state.setMeasurement("GNSS", "velE", velE);
        world.state.setMeasurement("GNSS", "velD", velD);
        CalculationEngine &engine = *world.engine;
        const QVector<double> &t = c.t;
        const int last = t.size() - 1;

        // The stencil's two samples at sample i
        const auto stencil = [last](int i) {
            return i == 0 ? std::pair<int, int>(0, 1)
                 : i == last ? std::pair<int, int>(last - 1, last)
                 : std::pair<int, int>(i - 1, i + 1);
        };
        const auto check = [&](const char *output, const QVector<double> &values, auto combine) {
            const QVector<double> actual = engine.measurement("GNSS", output);
            QVERIFY2(actual.size() == t.size(), qPrintable(QStringLiteral("%1 %2").arg(c.name, output)));
            for (int i = 0; i <= last; ++i) {
                const QString where = QStringLiteral("%1 %2[%3] = %4").arg(c.name, output).arg(i)
                                          .arg(actual[i], 0, 'g', 17);
                if (c.unavailable.contains(i)) {
                    QVERIFY2(std::isnan(actual[i]), qPrintable(where));
                } else {
                    const auto [a, b] = stencil(i);
                    const double expected = combine(values[a], values[b]) / (t[b] - t[a]);
                    QVERIFY2(actual[i] == expected, qPrintable(where));
                }
            }
        };
        const auto difference = [](double earlier, double later) { return later - earlier; };
        const auto rootSumSquare = [](double earlier, double later) {
            return std::sqrt(later * later + earlier * earlier);
        };
        check("accN", velN, difference);
        check("accE", velE, difference);
        check("accD", velD, difference);
        check("accAcc", sAcc, rootSumSquare);
        QCOMPARE(engine.undeclaredReadCount(), 0);
    }
}

// Acceptance 15: the descent pause is a declared preference input.
void BuiltinsEngineTest::analysisRangeFollowsPreference()
{
    World world;
    addPausedDescent(world.state);
    CalculationEngine &engine = *world.engine;

    QList<QSet<DependencyKey>> delivered;
    engine.setInvalidationListener([&delivered](const QSet<DependencyKey> &keys) {
        delivered.append(keys);
    });

    QCOMPARE(engine.attribute("_ANALYSIS_START_TIME").toDouble(), 0.0);
    QCOMPARE(engine.attribute("_ANALYSIS_END_TIME").toDouble(), 70.0);
    QCOMPARE(engine.attribute("_GROUND_ELEV").toDouble(), 600.0);
    QCOMPARE(engine.runCount("builtin.attr.analysisRange"), 1);

    world.prefs.set(world.registry, PreferenceKeys::ImportDescentPauseSeconds, 5.0);

    QCOMPARE(delivered.size(), 1);
    QVERIFY(delivered.first().contains(attr("_ANALYSIS_START_TIME")));
    QVERIFY(delivered.first().contains(attr("_ANALYSIS_END_TIME")));

    QCOMPARE(engine.attribute("_ANALYSIS_START_TIME").toDouble(), 45.0);
    QCOMPARE(engine.attribute("_ANALYSIS_END_TIME").toDouble(), 70.0);
    QCOMPARE(engine.attribute("_GROUND_ELEV").toDouble(), 600.0);
    QCOMPARE(engine.runCount("builtin.attr.analysisRange"), 2);
    QCOMPARE(engine.undeclaredReadCount(), 0);
}

void BuiltinsEngineTest::analysisRangeNeedsPreference()
{
    // No value for the declared preference: the input is unavailable, so the
    // calculation does not run.
    CalculationRegistry registry;
    registerBuiltInCalculations(registry);
    FakePreferenceProvider prefs;
    registry.setPreferenceProvider(&prefs);
    FakeSessionState state;
    addPausedDescent(state);
    CalculationEngine engine(&state, &registry);

    QVERIFY(!engine.attribute("_ANALYSIS_START_TIME").isValid());
    QVERIFY(!engine.attribute("_ANALYSIS_END_TIME").isValid());
    QVERIFY(engine.resultStatus("builtin.attr.analysisRange") == ResultStatus::MissingInput);
    QCOMPARE(engine.runCount("builtin.attr.analysisRange"), 0);
}

// The seven per-sensor candidates for _START_TIME / _DURATION are tried in
// registration order; the first whose sensor has a time axis wins.
void BuiltinsEngineTest::startTimeCandidateOrder()
{
    World world;
    addTimeData(world.state);
    world.state.setMeasurement("BARO", "time", {10, 20, 30});
    CalculationEngine &engine = *world.engine;

    QCOMPARE(engine.attribute("_START_TIME").toDouble(), T0 + 10.0);
    QCOMPARE(engine.attribute("_DURATION").toDouble(), 20.0);

    QVERIFY(engine.resultStatus("builtin.attr.timeExtent.GNSS") == ResultStatus::MissingInput);
    QVERIFY(engine.resultStatus("builtin.attr.timeExtent.BARO") == ResultStatus::Ok);
    QCOMPARE(engine.runCount("builtin.attr.timeExtent.BARO"), 1);
    // Later candidates were never looked at.
    QVERIFY(!engine.resultStatus("builtin.attr.timeExtent.TIME").has_value());
    QCOMPARE(engine.runCount("builtin.attr.timeExtent.TIME"), 0);
}

// A partial result: the window is entered but its bottom is never crossed.
void BuiltinsEngineTest::wspPartial()
{
    CalculationEngine &engine = *m_world->engine;
    m_world->state.setAttribute(engine, "_WSP_BOTTOM_ALT", -50.0);

    const QStringList unavailable = {"_WSP_EXIT_TIME", "_WSP_EXIT_LAT", "_WSP_EXIT_LON",
                                     "_WSP_TIME_RESULT", "_WSP_DIST_RESULT",
                                     "_WSP_SPEED_RESULT", "_WSP_SEP_RESULT"};

    for (int pass = 0; pass < 2; ++pass) {
        QCOMPARE(engine.attribute("_WSP_ENTRY_TIME").toDouble(), T0 + 41.5);
        QVERIFY(qAbs(engine.attribute("_WSP_ENTRY_LAT").toDouble() - 45.00415) <= 1e-9);
        QCOMPARE(engine.attribute("_WSP_ENTRY_LON").toDouble(), -75.0);
        for (const QString &key : unavailable)
            QVERIFY2(!engine.attribute(key).isValid(), qPrintable(key));
    }

    QVERIFY(engine.resultStatus("builtin.wsp.results") == ResultStatus::Ok);
    QCOMPARE(engine.runCount("builtin.wsp.results"), 1);
    QCOMPARE(engine.cachedState(attr("_WSP_EXIT_TIME")), CalculationEngine::CachedState::Unavailable);
}

void BuiltinsEngineTest::spWindowStartIsOneRun()
{
    CalculationEngine &engine = *m_world->engine;

    QCOMPARE(engine.attribute("_SP_WINDOW_START_ALT").toDouble(), 3885.0);
    QCOMPARE(engine.attribute("_SP_WINDOW_START_TIME").toDouble(), T0 + 11.0);
    QCOMPARE(engine.runCount("builtin.sp.windowStart"), 1);
    QCOMPARE(engine.cycleCount(), 0);
}

void BuiltinsEngineTest::simplifiedRunsOnce()
{
    CalculationEngine &engine = *m_world->engine;

    for (int pass = 0; pass < 2; ++pass) {
        QCOMPARE(engine.measurement("Simplified", "hMSL"), QVector<double>({4000.0, 100.0}));
        QCOMPARE(engine.measurement("Simplified", "_time"), QVector<double>({T0, T0 + 295.0}));
        QCOMPARE(engine.measurement("Simplified", "lon"), QVector<double>({-75.0, -75.0}));
        QCOMPARE(engine.measurement("Simplified", "lat").size(), 2);
        QCOMPARE(engine.measurement("Simplified", "north").size(), 2);
        QCOMPARE(engine.measurement("Simplified", "east").size(), 2);
        QCOMPARE(engine.measurement("Simplified", "down").size(), 2);
    }
    QCOMPARE(engine.runCount("builtin.simplified.track"), 1);
    QCOMPARE(engine.measurementUnit("Simplified", "lat"), QString());
    QCOMPARE(engine.runCount("builtin.local.coordinates"), 1);
    QCOMPARE(engine.measurementUnit("Simplified", "north"), QString());
}

// One family registration, one instance per distinct expression.
void BuiltinsEngineTest::interpolationInstances()
{
    CalculationEngine &engine = *m_world->engine;
    const QString key = SessionData::interpolationKey("_EXIT_TIME", "GNSS", "_time", "hMSL");
    QCOMPARE(key, QStringLiteral("_EXIT_TIME:GNSS/_time/hMSL"));

    for (int i = 0; i < 3; ++i)
        QCOMPARE(engine.attribute(key).toDouble(), 4000.0);
    QCOMPARE(engine.runCountForInstance("builtin.interpolation#_EXIT_TIME:GNSS/_time/hMSL"), 1);

    QCOMPARE(engine.attribute("_WSP_ENTRY_TIME:GNSS/_time/hMSL").toDouble(), 2600.0);
    QCOMPARE(engine.runCount("builtin.interpolation"), 2);
    QCOMPARE(m_world->registry.memoizedInstanceCount("builtin.interpolation"), 2);

    // Overriding the time attribute invalidates the instance that uses it.
    const QSet<DependencyKey> invalidated = m_world->state.setAttribute(engine, "_EXIT_TIME", T0 + 41.5);
    QVERIFY(invalidated.contains(DependencyKey::attribute(key)));
    QCOMPARE(engine.attribute(key).toDouble(), 2600.0);
    QCOMPARE(engine.runCountForInstance("builtin.interpolation#_EXIT_TIME:GNSS/_time/hMSL"), 2);

    // Not the interpolation syntax: nothing offers to produce it.
    QVERIFY(!m_world->registry.hasCandidateFor(attr("a:b/c")));
    QVERIFY(m_world->registry.hasCandidateFor(attr("_EXIT_TIME")));
    QVERIFY(!m_world->registry.hasCandidateFor(measKey("GNSS", "_EXIT_TIME:GNSS/_time/hMSL")));
}

// An expression that cannot be interpolated is cached as unavailable; the old
// engine re-parsed and re-searched on every read.
void BuiltinsEngineTest::interpolationUnavailableIsCached()
{
    CalculationEngine &engine = *m_world->engine;
    const QStringList keys = {"_NOPE:GNSS/_time/hMSL", "_EXIT_TIME:GNSS/_time/nope",
                              "_EXIT_TIME:GNSS/hMSL"};

    for (const QString &key : keys)
        QVERIFY2(!engine.attribute(key).isValid(), qPrintable(key));
    for (const QString &key : keys)
        QCOMPARE(engine.cachedState(DependencyKey::attribute(key)),
                 CalculationEngine::CachedState::Unavailable);

    const int runs = engine.totalRunCount();
    m_world->state.resetReadCount();
    for (const QString &key : keys)
        QVERIFY2(!engine.attribute(key).isValid(), qPrintable(key));
    QCOMPARE(m_world->state.readCount(), 0);
    QCOMPARE(engine.totalRunCount(), runs);
}

// A marker strictly inside a hole of the time vector reads nothing, as one
// outside the samples does; at the two samples around the hole it reads them.
// GNSS/_time steps a second with a 5 s hole between T0 + 3 and T0 + 8.
void BuiltinsEngineTest::interpolationInsideHole()
{
    const QVector<double> t = {T0, T0 + 1.0, T0 + 2.0, T0 + 3.0, T0 + 8.0, T0 + 9.0, T0 + 10.0};
    const QVector<double> hMSL = {1000.0, 990.0, 980.0, 970.0, 920.0, 910.0, 900.0};
    const QString key = SessionData::interpolationKey("_M", "GNSS", "_time", "hMSL");

    const auto read = [&](double marker) {
        World world;
        world.state.setMeasurement("GNSS", "time", t);
        world.state.setMeasurement("GNSS", "hMSL", hMSL);
        world.state.setAttribute("_M", marker);
        return world.engine->attribute(key);
    };

    QVERIFY(!read(T0 + 5.0).isValid());         // inside the hole
    QVERIFY(!read(T0 + 3.5).isValid());
    QVERIFY(!read(T0 + 7.999).isValid());
    QCOMPARE(read(T0 + 3.0).toDouble(), 970.0); // the sample before it
    QCOMPARE(read(T0 + 8.0).toDouble(), 920.0); // the sample after it
    QCOMPARE(read(T0 + 1.5).toDouble(), 985.0); // between connected samples, as before
    QCOMPARE(read(T0 + 8.5).toDouble(), 915.0);
    QVERIFY(!read(T0 - 1.0).isValid());         // outside the samples, as before
    QVERIFY(!read(T0 + 11.0).isValid());
}

// The automatic ground elevation is hMSL read at the analysis end: nothing
// strictly inside a hole of GNSS/_time, the sample's value at the two samples
// around it, and between connected samples and outside the samples the
// answers without a hole (the interpolated, the first or the last elevation).
void BuiltinsEngineTest::groundElevationInsideHole()
{
    const QVector<double> t = {T0, T0 + 1.0, T0 + 2.0, T0 + 3.0, T0 + 8.0, T0 + 9.0, T0 + 10.0};
    const QVector<double> hMSL = {1000.0, 990.0, 980.0, 970.0, 920.0, 910.0, 900.0};

    const auto ground = [&](double analysisEnd) {
        World world;
        world.state.setMeasurement("GNSS", "time", t);
        world.state.setMeasurement("GNSS", "hMSL", hMSL);
        world.state.setAttribute(SessionKeys::AnalysisEndTime, analysisEnd);
        return world.engine->attribute(SessionKeys::GroundElev);
    };

    QVERIFY(!ground(T0 + 5.0).isValid());
    QVERIFY(!ground(T0 + 3.25).isValid());
    QCOMPARE(ground(T0 + 3.0).toDouble(), 970.0);
    QCOMPARE(ground(T0 + 8.0).toDouble(), 920.0);
    QCOMPARE(ground(T0 + 1.5).toDouble(), 985.0);
    QCOMPARE(ground(T0 - 5.0).toDouble(), 1000.0);
    QCOMPARE(ground(T0 + 20.0).toDouble(), 900.0);
}

// The exit is a crossing time, the exception to the continuity rule: a
// crossing of 10 m/s in a hole of GNSS/_time, or in the interval beside one,
// is placed by linear interpolation between the samples around it, and as the
// acceleration there has no value (its stencil would span the hole) the gate
// and the exit read the slope of that interval, (velD[i] - velD[i - 1]) /
// (t[i] - t[i - 1]). Never NaN: a candidate whose slope is under the 2.5 m/s^2
// gate is passed over, and without another the exit is unavailable. GNSS/_time
// steps a second with a 5 s hole between T0 + 3 and T0 + 8; every value is
// exact, so the exits are compared bit for bit with the hand-worked ones.
void BuiltinsEngineTest::exitCrossingBesideHole()
{
    const QVector<double> t = {T0, T0 + 1.0, T0 + 2.0, T0 + 3.0, T0 + 8.0, T0 + 9.0, T0 + 10.0};
    const QVector<double> sAcc(t.size(), 0.5);
    constexpr double none = std::numeric_limits<double>::quiet_NaN();
    struct Case {
        const char *name;
        QVector<double> velD;
        double exit;
    };
    const Case cases[] = {
        // 5 -> 25 across the hole: a quarter of the way, T0 + 4.25; slope 4,
        // so 10 / 4 = 2.5 s before it
        {"in the hole", {0, 1, 2, 5, 25, 30, 35}, T0 + 1.75},
        // 6 -> 14 just before it: T0 + 2.5, slope 8, 1.25 s before
        {"before the hole", {0, 2, 6, 14, 20, 25, 30}, T0 + 1.25},
        // 6 -> 14 just after it: T0 + 8.5, slope 8, 1.25 s before
        {"after the hole", {0, 0, 0, 0, 6, 14, 20}, T0 + 7.25},
        // 5 -> 15 across the hole has a slope of 2, under the gate; 15 -> 5
        // after it falls; 5 -> 15 at the end, away from the hole, reads the
        // derivative (0 and 10, so 5 at the crossing): T0 + 9.5 - 2
        {"under the gate", {0, 0, 0, 5, 15, 5, 15}, T0 + 7.5},
        // 5 -> 15 across the hole under the gate, and no other crossing
        {"no exit", {0, 0, 0, 5, 15, 20, 25}, none},
    };

    for (const Case &c : cases) {
        World world;
        world.state.setMeasurement("GNSS", "time", t);
        world.state.setMeasurement("GNSS", "velD", c.velD);
        world.state.setMeasurement("GNSS", "sAcc", sAcc);
        world.state.setAttribute(SessionKeys::AnalysisStartTime, T0);
        world.state.setAttribute(SessionKeys::AnalysisEndTime, T0 + 10.0);

        const QVariant exit = world.engine->attribute(SessionKeys::ExitTime);
        if (std::isnan(c.exit)) {
            QVERIFY2(!exit.isValid(), c.name);
        } else {
            QVERIFY2(exit.isValid() && std::isfinite(exit.toDouble()), c.name);
            QVERIFY2(exit.toDouble() == c.exit,
                     qPrintable(QStringLiteral("%1: %2, expected %3").arg(c.name)
                                    .arg(exit.toDouble() - T0, 0, 'g', 17).arg(c.exit - T0, 0, 'g', 17)));
        }
        QCOMPARE(world.engine->undeclaredReadCount(), 0);
    }
}

// An altitude marker is a crossing time too: a crossing in a hole of
// GNSS/_time, or beside one, is placed by linear interpolation between the
// two samples around it. Ground at 0 m, so z is hMSL; the times as above,
// and the crossings exact, so compared bit for bit.
void BuiltinsEngineTest::altitudeCrossingAcrossHole()
{
    World world;
    world.state.setMeasurement("GNSS", "time",
                               {T0, T0 + 1.0, T0 + 2.0, T0 + 3.0, T0 + 8.0, T0 + 9.0, T0 + 10.0});
    world.state.setMeasurement("GNSS", "hMSL", {1000.0, 990.0, 980.0, 970.0, 920.0, 910.0, 900.0});
    world.state.setAttribute(SessionKeys::GroundElev, 0.0);
    world.state.setAttribute(SessionKeys::AnalysisStartTime, T0);
    world.state.setAttribute(SessionKeys::AnalysisEndTime, T0 + 10.0);
    for (const auto &[key, metres] : {std::pair<const char *, double>("_ALTITUDE_945_M", 945.0),
                                      std::pair<const char *, double>("_ALTITUDE_975_M", 975.0),
                                      std::pair<const char *, double>("_ALTITUDE_915_M", 915.0)})
        QVERIFY(world.registry.registerCalculation(AltitudeMarkerManager::makeDescriptor(key, metres)));
    CalculationEngine &engine = *world.engine;

    // 970 -> 920 across the hole: halfway, T0 + 3 + 2.5
    QVERIFY(engine.attribute("_ALTITUDE_945_M").toDouble() == T0 + 5.5);
    // In the intervals just before and just after it
    QVERIFY(engine.attribute("_ALTITUDE_975_M").toDouble() == T0 + 2.5);
    QVERIFY(engine.attribute("_ALTITUDE_915_M").toDouble() == T0 + 8.5);
    QCOMPARE(engine.undeclaredReadCount(), 0);
}

void BuiltinsEngineTest::altitudeDescriptor()
{
    QCOMPARE(AltitudeMarkerManager::calculationId("_ALTITUDE_1000_M"),
             QStringLiteral("builtin.altitude._ALTITUDE_1000_M"));
    QVERIFY(m_world->registry.registerCalculation(
        AltitudeMarkerManager::makeDescriptor("_ALTITUDE_1000_M", 1000.0)));
    CalculationEngine &engine = *m_world->engine;

    // Ground at 100 m: 1000 m AGL is 1100 m MSL, halfway between rows 71 and 72.
    QCOMPARE(engine.attribute("_ALTITUDE_1000_M").toDouble(), T0 + 71.5);

    // Ground at 0 m: row 78 is exactly 1000 m, so the crossing is at its start.
    const QSet<DependencyKey> invalidated = m_world->state.setAttribute(engine, "_GROUND_ELEV", 0.0);
    QVERIFY(invalidated.contains(attr("_ALTITUDE_1000_M")));
    QCOMPARE(engine.attribute("_ALTITUDE_1000_M").toDouble(), T0 + 78.0);
    QCOMPARE(engine.runCount("builtin.altitude._ALTITUDE_1000_M"), 2);
    QCOMPARE(engine.undeclaredReadCount(), 0);
}

// ─────────────────────────────── column cache: closures and environment digests

// What a logbook column "IMU/wx at marker _M" can depend on, from the
// registrations alone. An edit of any of these names (and of nothing else)
// makes the model recompute that column.
void BuiltinsEngineTest::gyroColumnClosure()
{
    World world;
    const char column[] = "_M:IMU/_time/wx";

    const StaticDependencies deps = world.registry.staticDependencies(attr(column));
    QCOMPARE(deps.names, QSet<DependencyKey>({
        attr(column),
        attr("_M"),
        measKey("IMU", "_time"), measKey("IMU", "time"),
        attr("_TIME_FIT_A"), attr("_TIME_FIT_B"),
        measKey("TIME", "time"), measKey("TIME", "tow"), measKey("TIME", "week"),
        measKey("IMU", "wx"),
        attr("SCHEMA_VER"),
    }));
    QVERIFY(deps.preferences.isEmpty());
    QVERIFY(!deps.names.contains(attr("_DESCRIPTION")));

    // Nothing calculates the description: it depends on itself only.
    QCOMPARE(world.registry.staticDependencies(attr("_DESCRIPTION")).names,
             QSet<DependencyKey>({attr("_DESCRIPTION")}));

    // The exit time reaches the declared preference through the analysis range.
    QVERIFY(world.registry.staticDependencies(attr("_EXIT_TIME")).preferences
                .contains(QString(PreferenceKeys::ImportDescentPauseSeconds)));

    // Pure functions of the registrations
    QCOMPARE(world.engine->totalRunCount(), 0);
    QCOMPARE(world.state.readCount(), 0);
    QCOMPARE(world.prefs.readCount(), 0);

    QCOMPARE(world.registry.declaredPreferenceKeys(),
             QStringList({QString(PreferenceKeys::ImportDescentPauseSeconds)}));
}

// The column environment digest is per set of names: a registration or a
// preference changes the digest of exactly the names whose static closure it
// reaches, and the order of registrations counts only where it can decide
// which candidate wins.
void BuiltinsEngineTest::digestChanges()
{
    World a;
    World b;
    const QList<DependencyKey> extraName = {attr("_TEST_EXTRA")};
    const QList<DependencyKey> secondName = {attr("_TEST_SECOND")};
    const QList<DependencyKey> exitName = {attr("_EXIT_TIME")};
    const QList<DependencyKey> descriptionName = {attr("_DESCRIPTION")};

    const QString base = calculationEnvironmentDigest(extraName, a.registry);
    QCOMPARE(base.size(), 40);
    QVERIFY(QRegularExpression(QStringLiteral("^[0-9a-f]{40}$")).match(base).hasMatch());
    const QString exitBase = calculationEnvironmentDigest(exitName, a.registry);
    QVERIFY(exitBase != base);

    // Two registries built the same way
    QCOMPARE(calculationEnvironmentDigest(extraName, b.registry), base);
    QCOMPARE(calculationEnvironmentDigest(exitName, b.registry), exitBase);

    // One extra calculation (what a plugin would be): the names it provides
    // change, a name whose closure it does not reach does not
    CalculationDescriptor extra;
    extra.id = QStringLiteral("test.extra");
    extra.outputs = {attr("_TEST_EXTRA")};
    extra.compute = [](const EvaluationContext &) { return CalculationResult().setAttribute("_TEST_EXTRA", 1); };
    QVERIFY(b.registry.registerCalculation(extra));
    const QString withExtra = calculationEnvironmentDigest(extraName, b.registry);
    QVERIFY(withExtra != base);
    QCOMPARE(calculationEnvironmentDigest(exitName, b.registry), exitBase);
    QVERIFY(b.registry.unregister(extra.id, CalculationRegistry::Removal::Change));
    QCOMPARE(calculationEnvironmentDigest(extraName, b.registry), base);

    // A calculation that reads _DESCRIPTION reaches the closure of its own
    // output, never the one of _DESCRIPTION
    CalculationDescriptor reader = extra;
    reader.id = QStringLiteral("test.reader");
    reader.inputs = {CalcInput::attribute(QStringLiteral("_DESCRIPTION"))};
    const QString descriptionBase = calculationEnvironmentDigest(descriptionName, b.registry);
    QVERIFY(b.registry.registerCalculation(reader));
    QCOMPARE(calculationEnvironmentDigest(descriptionName, b.registry), descriptionBase);
    QVERIFY(calculationEnvironmentDigest(extraName, b.registry) != withExtra);
    QVERIFY(b.registry.unregister(reader.id, CalculationRegistry::Removal::Change));

    // Two registrations with different outputs, swapped: they are never
    // candidates for the same name, so nothing can tell the orders apart
    CalculationDescriptor second = extra;
    second.id = QStringLiteral("test.second");
    second.outputs = {attr("_TEST_SECOND")};
    second.compute = [](const EvaluationContext &) { return CalculationResult().setAttribute("_TEST_SECOND", 1); };
    QVERIFY(a.registry.registerCalculation(extra));
    QVERIFY(a.registry.registerCalculation(second));
    QVERIFY(b.registry.registerCalculation(second));
    QVERIFY(b.registry.registerCalculation(extra));
    QVERIFY(a.registry.registeredIds() != b.registry.registeredIds());
    const QList<DependencyKey> both = {attr("_TEST_EXTRA"), attr("_TEST_SECOND")};
    const QString withBoth = calculationEnvironmentDigest(both, a.registry);
    QCOMPARE(calculationEnvironmentDigest(both, b.registry), withBoth);
    QCOMPARE(calculationEnvironmentDigest(extraName, a.registry), withExtra);
    QCOMPARE(calculationEnvironmentDigest(extraName, b.registry), withExtra);
    QCOMPARE(calculationEnvironmentDigest(secondName, a.registry), calculationEnvironmentDigest(secondName, b.registry));

    // Two candidates for ONE output, swapped: the first one wins, so the
    // order is part of the environment of that name (and only of that one)
    CalculationDescriptor rival = extra;
    rival.id = QStringLiteral("test.rival");
    CalculationDescriptor rival2 = extra;
    rival2.id = QStringLiteral("test.rival2");
    QVERIFY(a.registry.registerCalculation(rival));
    QVERIFY(a.registry.registerCalculation(rival2));
    QVERIFY(b.registry.registerCalculation(rival2));
    QVERIFY(b.registry.registerCalculation(rival));
    QVERIFY(calculationEnvironmentDigest(extraName, a.registry) != calculationEnvironmentDigest(extraName, b.registry));
    QVERIFY(calculationEnvironmentDigest(extraName, a.registry) != withExtra);
    QCOMPARE(calculationEnvironmentDigest(secondName, a.registry), calculationEnvironmentDigest(secondName, b.registry));
    QVERIFY(b.registry.unregister(rival.id, CalculationRegistry::Removal::Change));
    QVERIFY(b.registry.unregister(rival2.id, CalculationRegistry::Removal::Change));
    QCOMPARE(calculationEnvironmentDigest(both, b.registry), withBoth);

    // Before or after a family that accepts the name is a difference too: an
    // interpolation key has the family as a candidate, and a plain
    // calculation declaring the same key is tried before or after it
    const QList<DependencyKey> gyroNames = logbookColumnNames(gyroColumn());
    QCOMPARE(gyroNames.size(), 1);
    CalculationDescriptor gyroShadow;
    gyroShadow.id = QStringLiteral("test.gyroShadow");
    gyroShadow.outputs = {gyroNames.first()};
    gyroShadow.compute = [](const EvaluationContext &) { return CalculationResult(); };
    World early;
    World late;
    const QString gyroBase = calculationEnvironmentDigest(gyroNames, late.registry);
    QVERIFY(early.registry.unregister(QStringLiteral("builtin.interpolation"), CalculationRegistry::Removal::Change));
    QVERIFY(early.registry.registerCalculation(gyroShadow));
    Calculations::registerInterpolationFamily(early.registry);
    QVERIFY(late.registry.registerCalculation(gyroShadow));
    QCOMPARE(early.registry.registeredIds().size(), late.registry.registeredIds().size());
    QVERIFY(calculationEnvironmentDigest(gyroNames, late.registry) != gyroBase);
    QVERIFY(calculationEnvironmentDigest(gyroNames, early.registry) != calculationEnvironmentDigest(gyroNames, late.registry));
    QCOMPARE(calculationEnvironmentDigest(exitName, early.registry), exitBase);

    // The declared preference: only the names whose closure reads it
    World c;
    QCOMPARE(calculationEnvironmentDigest(exitName, c.registry), exitBase);
    const QString gyroInC = calculationEnvironmentDigest(gyroNames, c.registry);
    c.prefs.set(PreferenceKeys::ImportDescentPauseSeconds, 5.0);
    QVERIFY(calculationEnvironmentDigest(exitName, c.registry) != exitBase);
    QCOMPARE(calculationEnvironmentDigest(gyroNames, c.registry), gyroInC);
    QCOMPARE(calculationEnvironmentDigest(descriptionName, c.registry), descriptionBase);
    c.prefs.set(PreferenceKeys::ImportDescentPauseSeconds, 30.0);
    QCOMPARE(calculationEnvironmentDigest(exitName, c.registry), exitBase);

    // The same value read back as text (QSettings from an INI file) is the
    // same environment.
    c.prefs.set(PreferenceKeys::ImportDescentPauseSeconds, QStringLiteral("30"));
    QCOMPARE(calculationEnvironmentDigest(exitName, c.registry), exitBase);

    // No engine ran anything for any of this
    QCOMPARE(a.engine->totalRunCount(), 0);
    QCOMPARE(c.engine->totalRunCount(), 0);
}

// Every registration's result version is part of the environment of the
// names it is a candidate for: a changed plug-in (its code identity) or a
// changed fit algorithm discards the cached values of the columns whose
// closure reaches it, like a registry change. An id without one hashes as
// "#<id>".
void BuiltinsEngineTest::digestCoversResultVersions()
{
    const auto constantCalculation = [](const QString &id, const QString &output, const QString &version) {
        CalculationDescriptor d;
        d.id = id;
        d.outputs = {DependencyKey::attribute(output)};
        d.resultVersion = version;
        d.compute = [output](const EvaluationContext &) { return CalculationResult().setAttribute(output, 1); };
        return d;
    };
    const QList<DependencyKey> pinned = {attr("_TEST_PINNED")};

    // The encoding, on a registry of one calculation and no preference provider
    {
        const auto sha1 = [](const QByteArray &bytes) {
            return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex());
        };
        CalculationRegistry registry;
        QCOMPARE(calculationEnvironmentDigest(pinned, registry), sha1("attribute:_TEST_PINNED\n"));
        QVERIFY(registry.registerCalculation(constantCalculation("test.pinned", "_TEST_PINNED", QString())));
        QCOMPARE(calculationEnvironmentDigest(pinned, registry), sha1("attribute:_TEST_PINNED\n#test.pinned\n"));
        QVERIFY(registry.unregister(QStringLiteral("test.pinned"), CalculationRegistry::Removal::Change));
        // The version is a backslash and a line feed between letters: escaped
        QVERIFY(registry.registerCalculation(constantCalculation("test.pinned", "_TEST_PINNED", "a\\b\nc")));
        QCOMPARE(calculationEnvironmentDigest(pinned, registry),
                 sha1("attribute:_TEST_PINNED\n#test.pinned#a\\\\b\\nc\n"));

        // Inputs are followed (sorted by name) and the declared preference
        // closes the text, empty without a provider
        CalculationDescriptor reader = constantCalculation("test.reader", "_TEST_READER", QString());
        reader.inputs = {CalcInput::attribute(QStringLiteral("_TEST_PINNED")),
                         CalcInput::measurement(QStringLiteral("S"), QStringLiteral("m")),
                         CalcInput::preference(QStringLiteral("test.pref"))};
        QVERIFY(registry.registerCalculation(reader));
        QCOMPARE(calculationEnvironmentDigest({attr("_TEST_READER")}, registry),
                 sha1("attribute:_TEST_PINNED\n#test.pinned#a\\\\b\\nc\n"
                      "attribute:_TEST_READER\n#test.reader\n"
                      "measurement:S/m\nno conversions\n"
                      "pref:test.pref=\n"));

        // A '/' in a sensor or measurement name is escaped, so the two fields
        // cannot run into each other
        const DependencyKey slashInSensor = DependencyKey::measurement(QStringLiteral("a/b"), QStringLiteral("c"));
        const DependencyKey slashInName = DependencyKey::measurement(QStringLiteral("a"), QStringLiteral("b/c"));
        QCOMPARE(calculationEnvironmentDigest({slashInSensor}, registry),
                 sha1("measurement:a\\/b/c\nno conversions\n"));
        QCOMPARE(calculationEnvironmentDigest({slashInName}, registry),
                 sha1("measurement:a/b\\/c\nno conversions\n"));
    }

    // On the built-ins: declaring one differs from declaring none, and each
    // version is its own environment
    World world;
    const QList<DependencyKey> versioned = {attr("_TEST_VERSIONED")};
    const QList<DependencyKey> exitName = {attr("_EXIT_TIME")};
    const QString base = calculationEnvironmentDigest(versioned, world.registry);
    const QString exitBase = calculationEnvironmentDigest(exitName, world.registry);
    const QString extraId = QStringLiteral("test.versioned");
    const auto reRegister = [&](const QString &version) {
        if (world.registry.contains(extraId)
            && !world.registry.unregister(extraId, CalculationRegistry::Removal::Change))
            return false;
        return world.registry.registerCalculation(constantCalculation(extraId, "_TEST_VERSIONED", version));
    };
    QVERIFY(reRegister(QString()));
    const QString unversioned = calculationEnvironmentDigest(versioned, world.registry);
    QVERIFY(unversioned != base);
    QVERIFY(reRegister(QStringLiteral("v1")));
    const QString v1 = calculationEnvironmentDigest(versioned, world.registry);
    QVERIFY(v1 != unversioned);
    QVERIFY(reRegister(QStringLiteral("v2")));
    const QString v2 = calculationEnvironmentDigest(versioned, world.registry);
    QVERIFY(v2 != v1);
    QVERIFY(v2 != unversioned);
    QCOMPARE(calculationEnvironmentDigest(exitName, world.registry), exitBase);     // not reached
    QVERIFY(reRegister(QStringLiteral("v1")));
    QCOMPARE(calculationEnvironmentDigest(versioned, world.registry), v1);
    QVERIFY(world.registry.unregister(extraId, CalculationRegistry::Removal::Change));
    QCOMPARE(calculationEnvironmentDigest(versioned, world.registry), base);

    // Two candidates for one output: the version of the second (never the
    // first tried) counts too
    const QList<DependencyKey> shared = {attr("_TEST_SHARED")};
    QVERIFY(world.registry.registerCalculation(constantCalculation("test.first", "_TEST_SHARED", "f1")));
    QVERIFY(world.registry.registerCalculation(constantCalculation("test.second", "_TEST_SHARED", "s1")));
    const QString secondS1 = calculationEnvironmentDigest(shared, world.registry);
    QVERIFY(world.registry.unregister(QStringLiteral("test.second"), CalculationRegistry::Removal::Change));
    QVERIFY(world.registry.registerCalculation(constantCalculation("test.second", "_TEST_SHARED", "s2")));
    QCOMPARE(world.registry.candidatesFor(attr("_TEST_SHARED")).size(), 2);
    QCOMPARE(world.registry.candidatesFor(attr("_TEST_SHARED")).first().instanceId, QStringLiteral("test.first"));
    QVERIFY(calculationEnvironmentDigest(shared, world.registry) != secondS1);

    // No engine ran anything for any of this
    QCOMPARE(world.engine->totalRunCount(), 0);
}

// The measurement names of a closure carry the conversion layer: the
// conversions accepting the name, and whether any is registered at all (the
// last one going hands every measurement with source data to the passthrough).
void BuiltinsEngineTest::digestCoversConversionLayer()
{
    const QList<DependencyKey> gyroNames = logbookColumnNames(gyroColumn());
    World world;
    const QString base = calculationEnvironmentDigest(gyroNames, world.registry);
    QVERIFY(world.registry.staticDependencies(gyroNames.first()).names.contains(measKey("IMU", "wx")));

    // Without the default conversion family the gyro rate reads through the
    // schema conversion alone
    QVERIFY(world.registry.unregister(QStringLiteral("builtin.conversion.default"), CalculationRegistry::Removal::Change));
    const QString schemaOnly = calculationEnvironmentDigest(gyroNames, world.registry);
    QVERIFY(schemaOnly != base);
    QVERIFY(world.registry.unregister(QStringLiteral("builtin.conversion.schema"), CalculationRegistry::Removal::Change));
    QVERIFY(!world.registry.hasSourceConversions());
    const QString passthrough = calculationEnvironmentDigest(gyroNames, world.registry);
    QVERIFY(passthrough != schemaOnly);

    // A name outside the conversion layer is not reached
    QCOMPARE(calculationEnvironmentDigest({attr("_DESCRIPTION")}, world.registry),
             calculationEnvironmentDigest({attr("_DESCRIPTION")}, World().registry));
}

// An altitude marker added while the application runs is registered after the
// existing ones; the next start registers all of them in ascending order. Both
// are the same environment for the marker's own name, and a name whose
// closure does not reach the markers never changes: the cached values of
// every other column stay valid throughout.
void BuiltinsEngineTest::digestSurvivesRuntimeAltitudeMarker()
{
    TestEnvironment::instance().registerBuiltIns();     // the application registry, as at startup
    CalculationRegistry &registry = CalculationRegistry::instance();
    const QStringList idsBefore = registry.registeredIds();
    const QList<DependencyKey> marker2000 = {attr("_ALTITUDE_2000_M")};
    const QList<DependencyKey> others = {attr("_EXIT_TIME"), attr("_DESCRIPTION"),
                                         logbookColumnNames(gyroColumn()).first()};
    const QString othersBefore = calculationEnvironmentDigest(others);
    const QString markerBefore = calculationEnvironmentDigest(marker2000);

    const auto altitudeIds = [&]() { return registry.registeredIds().mid(idsBefore.size()); };

    writeAltitudes({1000, 3000});
    auto manager = std::make_unique<AltitudeMarkerManager>();
    PreferencesManager::instance().setValue(PreferenceKeys::AltitudeMarkersUnits, QStringLiteral("Metric"));
    manager->refresh();
    QCOMPARE(altitudeIds(), QStringList({"builtin.altitude._ALTITUDE_1000_M", "builtin.altitude._ALTITUDE_3000_M"}));
    QVERIFY(calculationEnvironmentDigest({attr("_ALTITUDE_1000_M")}) != calculationEnvironmentDigest(marker2000));
    QCOMPARE(calculationEnvironmentDigest(marker2000), markerBefore);
    QCOMPARE(calculationEnvironmentDigest(others), othersBefore);

    // Added at run time (the preference change refreshes the manager): appended
    writeAltitudes({1000, 3000, 2000});
    QCOMPARE(altitudeIds(), QStringList({"builtin.altitude._ALTITUDE_1000_M", "builtin.altitude._ALTITUDE_3000_M",
                                         "builtin.altitude._ALTITUDE_2000_M"}));
    const QString atRuntime = calculationEnvironmentDigest(marker2000);
    QVERIFY(atRuntime != markerBefore);
    QCOMPARE(calculationEnvironmentDigest(others), othersBefore);

    // The next start: a new manager registers them in ascending order
    manager.reset();
    QCOMPARE(registry.registeredIds(), idsBefore);
    manager = std::make_unique<AltitudeMarkerManager>();
    manager->refresh();
    QCOMPARE(altitudeIds(), QStringList({"builtin.altitude._ALTITUDE_1000_M", "builtin.altitude._ALTITUDE_2000_M",
                                         "builtin.altitude._ALTITUDE_3000_M"}));
    QCOMPARE(calculationEnvironmentDigest(marker2000), atRuntime);
    QCOMPARE(calculationEnvironmentDigest(others), othersBefore);

    manager.reset();
    writeAltitudes({});
    QCOMPARE(registry.registeredIds(), idsBefore);
    QCOMPARE(calculationEnvironmentDigest(marker2000), markerBefore);
}

// Removing an altitude marker while the application runs is a registry change
// that reports the requested results it drops; destroying the manager
// (shutdown) removes its registrations as teardown and reports nothing, even
// while an engine still holds a result that looked the marker up.
//
// The reader reads _TEST_ALT_OR_DEFAULT, whose first candidate copies the
// marker and whose second is a constant. With no GNSS data the marker's
// calculation is passed over, so the reader is Ok (-1) and its stored copy
// lists what the marker's calculation looked up; removing the marker leaves
// every answer alone but makes that copy stale, so the result is dropped
// (CALCULATIONS.md, section 5).
void BuiltinsEngineTest::altitudeMarkerTeardownReportsNothing()
{
    TestEnvironment::instance().registerBuiltIns();     // the application registry, as at startup
    CalculationRegistry &registry = CalculationRegistry::instance();
    const QStringList idsBefore = registry.registeredIds();

    writeAltitudes({1000});
    auto manager = std::make_unique<AltitudeMarkerManager>();
    PreferencesManager::instance().setValue(PreferenceKeys::AltitudeMarkersUnits, QStringLiteral("Metric"));
    manager->refresh();
    QVERIFY(registry.contains(QStringLiteral("builtin.altitude._ALTITUDE_1000_M")));

    CalculationDescriptor copyMarker;
    copyMarker.id = QStringLiteral("test.altitudeOrDefault.marker");
    copyMarker.inputs = {CalcInput::attribute(QStringLiteral("_ALTITUDE_1000_M"))};
    copyMarker.outputs = {attr("_TEST_ALT_OR_DEFAULT")};
    copyMarker.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute(QStringLiteral("_TEST_ALT_OR_DEFAULT"),
                                                ctx.attribute(QStringLiteral("_ALTITUDE_1000_M")));
    };
    CalculationDescriptor fallback;
    fallback.id = QStringLiteral("test.altitudeOrDefault.default");
    fallback.outputs = {attr("_TEST_ALT_OR_DEFAULT")};
    fallback.compute = [](const EvaluationContext &) {
        return CalculationResult().setAttribute(QStringLiteral("_TEST_ALT_OR_DEFAULT"), -1);
    };
    const QStringList helperIds = {copyMarker.id, fallback.id};

    const QString readerId = QStringLiteral("test.readsAltitude");
    CalculationDescriptor reader;
    reader.id = readerId;
    reader.policy = EvaluationPolicy::Explicit;
    reader.inputs = {CalcInput::attribute(QStringLiteral("_TEST_ALT_OR_DEFAULT"))};
    reader.outputs = {attr("_TEST_READS_ALTITUDE")};
    reader.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute(QStringLiteral("_TEST_READS_ALTITUDE"),
                                                ctx.attribute(QStringLiteral("_TEST_ALT_OR_DEFAULT")));
    };
    QVERIFY(registry.registerCalculation(copyMarker));
    QVERIFY(registry.registerCalculation(fallback));
    QVERIFY(registry.registerCalculation(reader));
    // Declared before the engine: runs after it on every exit
    auto unregisterReader = qScopeGuard([&registry, readerId, helperIds] {
        registry.unregister(readerId, CalculationRegistry::Removal::Change);
        for (const QString &id : helperIds)
            registry.unregister(id, CalculationRegistry::Removal::Change);
    });

    FakeSessionState state;     // empty: no GNSS data, so the altitude calculation cannot run
    QStringList events;
    auto engine = std::make_unique<CalculationEngine>(&state, &registry);
    engine->setExplicitResultListener([&events](const CalculationEngine::ExplicitResultEvent &event) {
        const QString kind = event.kind == CalculationEngine::ExplicitResultEvent::Kind::Installed
                                 ? QStringLiteral("Installed") : QStringLiteral("Dropped");
        const QString status = event.status == ResultStatus::Ok ? QStringLiteral("Ok")
                                                                : QString::number(int(event.status));
        events.append(kind + QLatin1Char(' ') + event.instanceId + QLatin1Char(' ') + status);
    });

    QCOMPARE(engine->request(readerId).status, ResultStatus::Ok);
    QCOMPARE(engine->attribute(QStringLiteral("_TEST_READS_ALTITUDE")), QVariant(-1));
    QCOMPARE(std::exchange(events, {}), QStringList({"Installed test.readsAltitude Ok"}));

    // A runtime removal (the manager refreshes): reported
    writeAltitudes({});
    QVERIFY(!registry.contains(QStringLiteral("builtin.altitude._ALTITUDE_1000_M")));
    QCOMPARE(std::exchange(events, {}), QStringList({"Dropped test.readsAltitude Ok"}));
    QVERIFY(!engine->resultStatus(readerId).has_value());

    // The teardown: nothing reported
    writeAltitudes({1000});
    QCOMPARE(engine->request(readerId).status, ResultStatus::Ok);
    QCOMPARE(std::exchange(events, {}), QStringList({"Installed test.readsAltitude Ok"}));
    manager.reset();
    QVERIFY(!registry.contains(QStringLiteral("builtin.altitude._ALTITUDE_1000_M")));
    QVERIFY(events.isEmpty());
    QVERIFY(!engine->resultStatus(readerId).has_value());

    engine.reset();
    QVERIFY(registry.unregister(readerId, CalculationRegistry::Removal::Change));
    for (const QString &id : helperIds)
        QVERIFY(registry.unregister(id, CalculationRegistry::Removal::Change));
    unregisterReader.dismiss();
    writeAltitudes({});
    QCOMPARE(registry.registeredIds(), idsBefore);
}

FLYSIGHT_TEST_MAIN(BuiltinsEngineTest)
#include "tst_builtins_engine.moc"
