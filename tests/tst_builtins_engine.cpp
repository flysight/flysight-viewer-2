// The built-in calculations on the calculation engine, against a fake session
// state and a private registry: golden values, the registration inventory,
// declared inputs only, multi-output groups, ordered candidates, the declared
// preference input, the interpolation family, and the altitude descriptor.

#include <memory>

#include <QtTest>

#include "altitudemarkerfeature.h"
#include "builtinfixture.h"
#include "calculations/builtincalculations.h"
#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "fakesessionstate.h"
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

    void timeFitRunsOnce();

    void analysisRangeFollowsPreference();
    void analysisRangeNeedsPreference();
    void startTimeCandidateOrder();

    void wspPartial();
    void spWindowStartIsOneRun();
    void simplifiedRunsOnce();

    void interpolationInstances();
    void interpolationUnavailableIsCached();
    void altitudeDescriptor();

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
        // attributecalculations
        "builtin.attr.analysisRange",
        "builtin.attr.exitTime",
        "builtin.attr.syncTime",
        "builtin.attr.courseRef",
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
        // simplificationcalculations
        "builtin.simplified.track",
        // wspcalculations
        "builtin.wsp.default.version",
        "builtin.wsp.default.topAlt",
        "builtin.wsp.default.bottomAlt",
        "builtin.wsp.default.task",
        "builtin.wsp.ref1Time",
        "builtin.wsp.results",
        // spcalculations
        "builtin.sp.default.perfWindowHeight",
        "builtin.sp.default.valWindowHeight",
        "builtin.sp.default.breakoffAlt",
        "builtin.sp.windowStart",
        "builtin.sp.results",
        // synthesized interpolation, last
        "builtin.interpolation",
    };

    const QStringList ids = m_world->registry.registeredIds();
    QCOMPARE(ids, expected);
    QCOMPARE(ids.size(), 68);   // 67 calculations + 1 family

    for (const QString &id : ids)
        QCOMPARE(m_world->registry.isFamily(id), id == QStringLiteral("builtin.interpolation"));

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
    QCOMPARE(plain, 67);

    QCOMPARE(engine.undeclaredReadCount(), 0);
    QCOMPARE(engine.cycleCount(), 0);
    QCOMPARE(engine.scopeDepth(), 0);

    // On the full fixture every built-in has its inputs.
    for (const QString &id : m_world->registry.registeredIds()) {
        if (!m_world->registry.isFamily(id))
            QVERIFY2(engine.resultStatus(id) == ResultStatus::Ok, qPrintable(id));
    }
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

// Spec 7.8 / acceptance 15: the descent pause is a declared preference input.
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
    }
    QCOMPARE(engine.runCount("builtin.simplified.track"), 1);
    QCOMPARE(engine.measurementUnit("Simplified", "lat"), QString());
}

// Spec 7.6: one family registration, one instance per distinct expression.
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

FLYSIGHT_TEST_MAIN(BuiltinsEngineTest)
#include "tst_builtins_engine.moc"
