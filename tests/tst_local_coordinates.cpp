// The recording-wide local north/east/down frame (builtin.local.coordinates
// and the two Local time axes): origin gates, the transform and the velocity
// rotation against analytically known WGS84 cases, NaN at the index of an
// invalid sample only, all outputs unavailable without a qualifying fix, and
// invalidation on source changes. Engine-level functions use a fake session
// state and a private registry; the last two use a real SessionData with the
// built-ins in the process-wide registry. No expectation is computed with
// GeographicLib or with the code under test.

#include <cmath>
#include <limits>
#include <memory>

#include <QtTest>

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

constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
constexpr double Inf = std::numeric_limits<double>::infinity();

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

// TIME/time, tow, week whose fit is exactly a = 1, b = 1704110400.
void addTimeData(FakeSessionState &state)
{
    state.setMeasurement("TIME", "time", {10, 20, 30});
    state.setMeasurement("TIME", "tow", {129610, 129620, 129630});
    state.setMeasurement("TIME", "week", {2295, 2295, 2295});
}

// A GNSS track at one fix per second. The velocity is zero unless given.
void addTrack(FakeSessionState &state, const QVector<double> &lat, const QVector<double> &lon,
              const QVector<double> &hMSL, const QVector<double> &hAcc,
              QVector<double> velN = {}, QVector<double> velE = {}, QVector<double> velD = {})
{
    const int n = lat.size();
    QVector<double> time;
    for (int i = 0; i < n; ++i)
        time.append(1704110400.0 + i);

    state.setMeasurement("GNSS", "time", time);
    state.setMeasurement("GNSS", "lat", lat);
    state.setMeasurement("GNSS", "lon", lon);
    state.setMeasurement("GNSS", "hMSL", hMSL);
    state.setMeasurement("GNSS", "hAcc", hAcc);
    state.setMeasurement("GNSS", "velN", velN.isEmpty() ? QVector<double>(n, 0.0) : velN);
    state.setMeasurement("GNSS", "velE", velE.isEmpty() ? QVector<double>(n, 0.0) : velE);
    state.setMeasurement("GNSS", "velD", velD.isEmpty() ? QVector<double>(n, 0.0) : velD);
}

// The origin, a fix 100 m above it, a fix a quarter of the equator to the
// east, and a fix 0.001 degrees to the north.
void addEquatorTrack(FakeSessionState &state, double velN = 0.0, double velE = 0.0, double velD = 0.0)
{
    addTrack(state, {0, 0, 0, 0.001}, {0, 0, 90, 0}, {0, 100, 0, 0}, {1, 1, 1, 1},
             QVector<double>(4, velN), QVector<double>(4, velE), QVector<double>(4, velD));
}

const QList<DependencyKey> originAttributes = {
    attr("_LOCAL_ORIGIN_LAT"), attr("_LOCAL_ORIGIN_LON"),
    attr("_LOCAL_ORIGIN_HMSL"), attr("_LOCAL_ORIGIN_INDEX")};

const QList<DependencyKey> localChannels = {
    measKey("Local", "north"), measKey("Local", "east"), measKey("Local", "down"),
    measKey("Local", "velN"), measKey("Local", "velE"), measKey("Local", "velD")};

bool containsAnyLocalName(const QSet<DependencyKey> &keys)
{
    for (const DependencyKey &key : keys) {
        if (originAttributes.contains(key))
            return true;
        if (key.type == DependencyKey::Type::Measurement
            && key.measurementKey.first == QLatin1String("Local"))
            return true;
    }
    return false;
}

bool containsEveryFrameOutput(const QSet<DependencyKey> &keys)
{
    for (const DependencyKey &key : originAttributes + localChannels) {
        if (!keys.contains(key))
            return false;
    }
    return true;
}

bool isNear(double actual, double expected, double tolerance)
{
    return std::abs(actual - expected) <= tolerance;
}

} // namespace

class LocalCoordinatesTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void originGates();
    void originBoundsAreInclusive();
    void speedAccuracyPlaysNoPart();
    void knownDisplacement();
    void knownVelocityRotation();
    void invalidSamplesAreNaNAtTheirIndexOnly();
    void noQualifyingFixMakesEverythingUnavailable();
    void runsOnceForAllOutputs();
    void timeAxesAreTheGnssAxes();

    // Session level: a real SessionData, the built-ins in the global registry
    void sourceChangesInvalidate();
    void markersAndDisplayDoNotAffectTheFrame();

private:
    QStringList m_registryBefore;
};

void LocalCoordinatesTest::initTestCase()
{
    TestEnvironment::instance().registerBuiltIns();
}

void LocalCoordinatesTest::init()
{
    TestEnvironment::instance().resetPreferencesToDefaults();
    m_registryBefore = CalculationRegistry::instance().registeredIds();
}

// The global registry is left as it was found.
void LocalCoordinatesTest::cleanup()
{
    QCOMPARE(CalculationRegistry::instance().registeredIds(), m_registryBefore);
}

// The origin is the first fix that passes every gate: hAcc finite and in
// [0, 10), latitude and longitude on the globe, hMSL finite. Fixes 0-3 fail
// on accuracy, 4-6 on position.
void LocalCoordinatesTest::originGates()
{
    World world;
    addTrack(world.state,
             {1, 2, 3, 4, 91, 6, 7, 0},
             {1, 2, 3, 4, 5, 181, 7, 0},
             {10, 20, 30, 40, 50, 60, NaN, 100},
             {10, -1, NaN, Inf, 1, 1, 1, 0});
    CalculationEngine &engine = *world.engine;

    const QVariant index = engine.attribute("_LOCAL_ORIGIN_INDEX");
    QCOMPARE(index.userType(), int(QMetaType::LongLong));
    QCOMPARE(index.toLongLong(), 7LL);

    const QVariant hMSL = engine.attribute("_LOCAL_ORIGIN_HMSL");
    QCOMPARE(hMSL.userType(), int(QMetaType::Double));
    QVERIFY(hMSL.toDouble() == 100.0);
    QVERIFY(engine.attribute("_LOCAL_ORIGIN_LAT").toDouble() == 0.0);
    QVERIFY(engine.attribute("_LOCAL_ORIGIN_LON").toDouble() == 0.0);

    // Accuracy selects the origin only: a fix with a poor or non-finite hAcc
    // and a valid position still has coordinates.
    const QVector<double> north = engine.measurement("Local", "north");
    QCOMPARE(north.size(), 8);
    for (int i : {0, 1, 2, 3, 7})
        QVERIFY2(std::isfinite(north[i]), qPrintable(QString::number(i)));
    for (int i : {4, 5, 6})
        QVERIFY2(std::isnan(north[i]), qPrintable(QString::number(i)));

    QCOMPARE(engine.undeclaredReadCount(), 0);
}

// The pole, the antimeridian, and hAcc = 0 are all inside the gates.
void LocalCoordinatesTest::originBoundsAreInclusive()
{
    World world;
    addTrack(world.state, {90, 45}, {-180, 10}, {0, 0}, {0, 1});
    CalculationEngine &engine = *world.engine;

    QCOMPARE(engine.attribute("_LOCAL_ORIGIN_INDEX").toLongLong(), 0LL);
    QVERIFY(engine.attribute("_LOCAL_ORIGIN_LAT").toDouble() == 90.0);
    QVERIFY(engine.attribute("_LOCAL_ORIGIN_LON").toDouble() == -180.0);
    QCOMPARE(engine.undeclaredReadCount(), 0);
}

// GNSS/sAcc is not an input: whatever it is, the frame neither moves nor reruns.
void LocalCoordinatesTest::speedAccuracyPlaysNoPart()
{
    World world;
    addTrack(world.state, {45, 45, 45}, {-75, -75, -75}, {1000, 990, 980}, {20, 1, 1});
    CalculationEngine &engine = *world.engine;

    QCOMPARE(engine.attribute("_LOCAL_ORIGIN_INDEX").toLongLong(), 1LL);
    QCOMPARE(engine.measurement("Local", "down").size(), 3);
    QCOMPARE(engine.runCount("builtin.local.coordinates"), 1);

    for (double sAcc : {Inf, NaN}) {
        const QSet<DependencyKey> invalidated =
            world.state.setMeasurement(engine, "GNSS", "sAcc", QVector<double>(3, sAcc));
        QVERIFY(!containsAnyLocalName(invalidated));

        QCOMPARE(engine.attribute("_LOCAL_ORIGIN_INDEX").toLongLong(), 1LL);
        QCOMPARE(engine.measurement("Local", "down").size(), 3);
        QCOMPARE(engine.runCount("builtin.local.coordinates"), 1);
    }
    QCOMPARE(engine.undeclaredReadCount(), 0);
}

// Analytic cases on WGS84 (a = 6378137 m) with the origin at (0, 0, 0).
void LocalCoordinatesTest::knownDisplacement()
{
    World world;
    addEquatorTrack(world.state);
    CalculationEngine &engine = *world.engine;

    const QVector<double> north = engine.measurement("Local", "north");
    const QVector<double> east = engine.measurement("Local", "east");
    const QVector<double> down = engine.measurement("Local", "down");
    QCOMPARE(north.size(), 4);
    QCOMPARE(east.size(), 4);
    QCOMPARE(down.size(), 4);

    // The origin itself
    QVERIFY(isNear(north[0], 0.0, 1e-8));
    QVERIFY(isNear(east[0], 0.0, 1e-8));
    QVERIFY(isNear(down[0], 0.0, 1e-8));

    // 100 m straight up
    QVERIFY(isNear(north[1], 0.0, 1e-8));
    QVERIFY(isNear(east[1], 0.0, 1e-8));
    QVERIFY(isNear(down[1], -100.0, 1e-8));

    // A quarter of the equator: the origin's east axis points at it, and it
    // lies one radius below the origin's tangent plane.
    QVERIFY(isNear(north[2], 0.0, 1e-6));
    QVERIFY(isNear(east[2], 6378137.0, 1e-6));
    QVERIFY(isNear(down[2], 6378137.0, 1e-6));

    // 0.001 degrees north: the meridian radius of curvature at the equator,
    // a (1 - e^2) = 6335439.327 m, times the angle in radians. The ground
    // curves away below the tangent plane by about a millimetre.
    QVERIFY2(isNear(north[3], 110.574276, 1e-3), qPrintable(QString::number(north[3], 'g', 17)));
    QVERIFY(isNear(east[3], 0.0, 1e-6));
    QVERIFY2(down[3] > 0.0 && down[3] < 0.01, qPrintable(QString::number(down[3], 'g', 17)));

    QCOMPARE(engine.undeclaredReadCount(), 0);
}

// The recorded velocity is north/east/down at each fix; the output is in the
// origin's axes. At (0, 90, 0) the fix's north is the origin's north, its east
// is the origin's down, and its down is the origin's negative east.
void LocalCoordinatesTest::knownVelocityRotation()
{
    World world;
    addEquatorTrack(world.state, 2.0, 3.0, 4.0);
    CalculationEngine &engine = *world.engine;

    const QVector<double> velN = engine.measurement("Local", "velN");
    const QVector<double> velE = engine.measurement("Local", "velE");
    const QVector<double> velD = engine.measurement("Local", "velD");
    QCOMPARE(velN.size(), 4);
    QCOMPARE(velE.size(), 4);
    QCOMPARE(velD.size(), 4);

    QVERIFY(isNear(velN[0], 2.0, 1e-12));
    QVERIFY(isNear(velE[0], 3.0, 1e-12));
    QVERIFY(isNear(velD[0], 4.0, 1e-12));

    QVERIFY(isNear(velN[2], 2.0, 1e-12));
    QVERIFY(isNear(velE[2], -4.0, 1e-12));
    QVERIFY(isNear(velD[2], 3.0, 1e-12));

    // The recorded channels are untouched
    QCOMPARE(engine.measurement("GNSS", "velN"), QVector<double>(4, 2.0));
    QCOMPARE(engine.measurement("GNSS", "velE"), QVector<double>(4, 3.0));
    QCOMPARE(engine.measurement("GNSS", "velD"), QVector<double>(4, 4.0));

    QCOMPARE(engine.undeclaredReadCount(), 0);
}

// One entry per GNSS sample, always. Sample 1 has no position, sample 3 no
// velocity; sample 2 is directly above the origin, which shows that nothing
// was shortened or shifted.
void LocalCoordinatesTest::invalidSamplesAreNaNAtTheirIndexOnly()
{
    World world;
    addTrack(world.state,
             {45, NaN, 45, 45.001, 45.002},
             {-75, -75, -75, -75, -75},
             {1000, 990, 1250, 970, 960},
             {1, 1, 1, 1, 1},
             {5, 5, 5, 5, 5},
             {1, 1, 1, NaN, 1},
             {9, 9, 9, 9, 9});
    CalculationEngine &engine = *world.engine;

    const QStringList positions = {"north", "east", "down"};
    const QStringList velocities = {"velN", "velE", "velD"};
    for (const QString &name : positions + velocities) {
        const QVector<double> values = engine.measurement("Local", name);
        QVERIFY2(values.size() == 5, qPrintable(name));

        for (int i : {0, 2, 4})
            QVERIFY2(std::isfinite(values[i]), qPrintable(name));
        QVERIFY2(std::isnan(values[1]), qPrintable(name));
        if (velocities.contains(name))
            QVERIFY2(std::isnan(values[3]), qPrintable(name));
        else
            QVERIFY2(std::isfinite(values[3]), qPrintable(name));
    }

    const QVector<double> down = engine.measurement("Local", "down");
    QVERIFY2(isNear(down[2], -250.0, 1e-8), qPrintable(QString::number(down[2], 'g', 17)));
    QVERIFY(isNear(engine.measurement("Local", "north")[2], 0.0, 1e-8));
    QVERIFY(engine.measurement("Local", "north")[3] > 100.0);

    QCOMPARE(engine.runCount("builtin.local.coordinates"), 1);
    QCOMPARE(engine.undeclaredReadCount(), 0);
}

void LocalCoordinatesTest::noQualifyingFixMakesEverythingUnavailable()
{
    const auto verifyNothingAvailable = [](CalculationEngine &engine) {
        for (const DependencyKey &name : originAttributes)
            QVERIFY2(!engine.attribute(name.attributeKey).isValid(), qPrintable(name.attributeKey));
        for (const DependencyKey &name : localChannels) {
            QVERIFY2(engine.measurement(name.measurementKey.first, name.measurementKey.second).isEmpty(),
                     qPrintable(name.measurementKey.second));
        }
    };

    // No fix reaches 10 m: the calculation ran and produced nothing, and that
    // answer is cached like any other.
    {
        World world;
        addTrack(world.state, {45, 45, 45}, {-75, -75, -75}, {1000, 990, 980}, {10, 10, 10});
        CalculationEngine &engine = *world.engine;

        verifyNothingAvailable(engine);
        QVERIFY(engine.resultStatus("builtin.local.coordinates") == ResultStatus::Ok);
        QCOMPARE(engine.runCount("builtin.local.coordinates"), 1);
        for (const DependencyKey &name : originAttributes + localChannels)
            QVERIFY(engine.cachedState(name) == CalculationEngine::CachedState::Unavailable);

        verifyNothingAvailable(engine);
        QCOMPARE(engine.runCount("builtin.local.coordinates"), 1);
        QCOMPARE(engine.undeclaredReadCount(), 0);
    }

    // No hAcc column at all: a missing input, so nothing runs.
    {
        World world;
        addTrack(world.state, {45, 45, 45}, {-75, -75, -75}, {1000, 990, 980}, {1, 1, 1});
        world.state.removeMeasurement("GNSS", "hAcc");
        CalculationEngine &engine = *world.engine;

        verifyNothingAvailable(engine);
        QVERIFY(engine.resultStatus("builtin.local.coordinates") == ResultStatus::MissingInput);
        QCOMPARE(engine.runCount("builtin.local.coordinates"), 0);
        QCOMPARE(engine.undeclaredReadCount(), 0);
    }

    // A ragged GNSS sensor has no frame.
    {
        World world;
        addTrack(world.state, {45, 45, 45}, {-75, -75, -75}, {1000, 990, 980}, {1, 1, 1});
        world.state.setMeasurement("GNSS", "hAcc", {1, 1});
        CalculationEngine &engine = *world.engine;

        verifyNothingAvailable(engine);
        QVERIFY(engine.resultStatus("builtin.local.coordinates") == ResultStatus::Ok);
        QCOMPARE(engine.runCount("builtin.local.coordinates"), 1);
        QCOMPARE(engine.undeclaredReadCount(), 0);
    }
}

// One calculation behind all ten names: whichever is read first, it runs once.
void LocalCoordinatesTest::runsOnceForAllOutputs()
{
    World world;
    addEquatorTrack(world.state);
    CalculationEngine &engine = *world.engine;

    for (int pass = 0; pass < 2; ++pass) {
        QCOMPARE(engine.measurement("Local", "velE").size(), 4);
        QVERIFY(engine.attribute("_LOCAL_ORIGIN_HMSL").isValid());
        QCOMPARE(engine.measurement("Local", "down").size(), 4);
        QVERIFY(engine.attribute("_LOCAL_ORIGIN_INDEX").isValid());
        QCOMPARE(engine.measurement("Local", "north").size(), 4);
        QCOMPARE(engine.measurement("Local", "velD").size(), 4);
        QVERIFY(engine.attribute("_LOCAL_ORIGIN_LON").isValid());
        QCOMPARE(engine.measurement("Local", "east").size(), 4);
        QVERIFY(engine.attribute("_LOCAL_ORIGIN_LAT").isValid());
        QCOMPARE(engine.measurement("Local", "velN").size(), 4);
        QCOMPARE(engine.runCount("builtin.local.coordinates"), 1);
    }
    QCOMPARE(engine.undeclaredReadCount(), 0);
}

// Local has no clock of its own: both axes are the GNSS ones. The system-time
// axis needs the TIME sensor's fit; the positions do not.
void LocalCoordinatesTest::timeAxesAreTheGnssAxes()
{
    {
        World world;
        addEquatorTrack(world.state);
        addTimeData(world.state);
        CalculationEngine &engine = *world.engine;

        const QVector<double> utc = {1704110400.0, 1704110401.0, 1704110402.0, 1704110403.0};
        QCOMPARE(engine.measurement("Local", "_time"), utc);
        QCOMPARE(engine.measurement("Local", "_time"), engine.measurement("GNSS", "_time"));

        QCOMPARE(engine.measurement("Local", "_system_time"), QVector<double>({0.0, 1.0, 2.0, 3.0}));
        QCOMPARE(engine.measurement("Local", "_system_time"), engine.measurement("GNSS", "_system_time"));
        QCOMPARE(engine.undeclaredReadCount(), 0);
    }
    {
        World world;
        addEquatorTrack(world.state);
        CalculationEngine &engine = *world.engine;

        QCOMPARE(engine.measurement("Local", "_time").size(), 4);
        QVERIFY(engine.measurement("Local", "_system_time").isEmpty());
        QVERIFY(engine.resultStatus("builtin.local.systemTime") == ResultStatus::MissingInput);
        QCOMPARE(engine.measurement("Local", "north").size(), 4);
        QCOMPARE(engine.undeclaredReadCount(), 0);
    }
}

namespace {

// Four fixes on one vertical line, 100 m apart, climbing.
void addClimb(SessionData &session, const QVector<double> &hAcc)
{
    session.setMeasurement("GNSS", "time", {1704110400.0, 1704110401.0, 1704110402.0, 1704110403.0});
    session.setMeasurement("GNSS", "lat", {45, 45, 45, 45});
    session.setMeasurement("GNSS", "lon", {-75, -75, -75, -75});
    session.setMeasurement("GNSS", "hMSL", {1000, 1100, 1200, 1300});
    session.setMeasurement("GNSS", "hAcc", hAcc);
    session.setMeasurement("GNSS", "velN", {0, 0, 0, 0});
    session.setMeasurement("GNSS", "velE", {0, 0, 0, 0});
    session.setMeasurement("GNSS", "velD", {-100, -100, -100, -100});
}

void readEveryFrameOutput(const SessionData &session)
{
    for (const DependencyKey &name : originAttributes)
        session.getAttribute(name.attributeKey);
    for (const DependencyKey &name : localChannels)
        session.getMeasurement(name.measurementKey.first, name.measurementKey.second);
}

bool downIs(const SessionData &session, const QVector<double> &expected)
{
    const QVector<double> down = session.getMeasurement("Local", "down");
    if (down.size() != expected.size())
        return false;
    for (int i = 0; i < down.size(); ++i) {
        if (!isNear(down[i], expected[i], 1e-8))
            return false;
    }
    return true;
}

} // namespace

// Acceptance: source changes invalidate the outputs. The frame follows the
// GNSS data, all ten outputs go together, and a recording that loses its
// origin gets it back when the source is corrected.
void LocalCoordinatesTest::sourceChangesInvalidate()
{
    SessionData session;
    addClimb(session, {1, 1, 1, 1});
    CalculationEngine &engine = session.calculationEngine();

    readEveryFrameOutput(session);
    QCOMPARE(session.getAttribute("_LOCAL_ORIGIN_INDEX").toLongLong(), 0LL);
    QVERIFY(downIs(session, {0.0, -100.0, -200.0, -300.0}));
    QCOMPARE(engine.runCount("builtin.local.coordinates"), 1);

    // The first fix no longer qualifies: the origin moves up 100 m, and the
    // old origin sample is now 100 m below it.
    QSet<DependencyKey> invalidated = session.setMeasurement("GNSS", "hAcc", {10, 1, 1, 1});
    QVERIFY(containsEveryFrameOutput(invalidated));

    // Siblings go together: the ten outputs are one result, so a velocity
    // change takes the positions with it. The engine reports the names that
    // had been resolved since the last run (here Local/north alone); a name
    // nobody has read has no reader to tell, and is simply computed afresh.
    QCOMPARE(session.getMeasurement("Local", "north").size(), 4);
    QCOMPARE(engine.runCount("builtin.local.coordinates"), 2);
    invalidated = session.setMeasurement("GNSS", "velE", {1, 1, 1, 1});
    QVERIFY(invalidated.contains(measKey("Local", "north")));
    QVERIFY(engine.cachedState(measKey("Local", "north")) == CalculationEngine::CachedState::NotCached);
    QVERIFY(engine.cachedState(measKey("Local", "velD")) == CalculationEngine::CachedState::NotCached);
    QVERIFY(!engine.resultStatus("builtin.local.coordinates").has_value());

    QCOMPARE(session.getAttribute("_LOCAL_ORIGIN_INDEX").toLongLong(), 1LL);
    QVERIFY(session.getAttribute("_LOCAL_ORIGIN_HMSL").toDouble() == 1100.0);
    QVERIFY(downIs(session, {100.0, 0.0, -100.0, -200.0}));
    QCOMPARE(session.getMeasurement("Local", "velE"), QVector<double>({1, 1, 1, 1}));
    QCOMPARE(engine.runCount("builtin.local.coordinates"), 3);

    // No qualifying fix: everything becomes unavailable ...
    readEveryFrameOutput(session);
    QCOMPARE(engine.runCount("builtin.local.coordinates"), 3);
    invalidated = session.setMeasurement("GNSS", "hAcc", {10, 10, 10, 10});
    QVERIFY(containsEveryFrameOutput(invalidated));
    for (const DependencyKey &name : originAttributes)
        QVERIFY2(!session.getAttribute(name.attributeKey).isValid(), qPrintable(name.attributeKey));
    for (const DependencyKey &name : localChannels) {
        QVERIFY2(session.getMeasurement(name.measurementKey.first, name.measurementKey.second).isEmpty(),
                 qPrintable(name.measurementKey.second));
    }
    QVERIFY(engine.resultStatus("builtin.local.coordinates") == ResultStatus::Ok);
    QCOMPARE(engine.runCount("builtin.local.coordinates"), 4);

    // ... and comes back when the source is corrected.
    invalidated = session.setMeasurement("GNSS", "hAcc", {1, 1, 1, 1});
    QVERIFY(containsEveryFrameOutput(invalidated));
    QCOMPARE(session.getAttribute("_LOCAL_ORIGIN_INDEX").toLongLong(), 0LL);
    QVERIFY(downIs(session, {0.0, -100.0, -200.0, -300.0}));
    QCOMPARE(session.getMeasurement("Local", "velD").size(), 4);
    QCOMPARE(engine.runCount("builtin.local.coordinates"), 5);

    QCOMPARE(engine.undeclaredReadCount(), 0);
    QCOMPARE(engine.cycleCount(), 0);
}

// Markers and the ground elevation are not inputs of the frame, and
// calculated outputs never show up as stored data.
void LocalCoordinatesTest::markersAndDisplayDoNotAffectTheFrame()
{
    SessionData session;
    addClimb(session, {1, 1, 1, 1});
    CalculationEngine &engine = session.calculationEngine();

    readEveryFrameOutput(session);
    QVERIFY(downIs(session, {0.0, -100.0, -200.0, -300.0}));
    QCOMPARE(engine.runCount("builtin.local.coordinates"), 1);

    QVERIFY(!containsAnyLocalName(session.setAttribute(SessionKeys::ExitTime, 1704110401.0)));
    QVERIFY(!containsAnyLocalName(session.setAttribute(SessionKeys::AnalysisStartTime, 1704110401.0)));
    QVERIFY(!containsAnyLocalName(session.setAttribute(SessionKeys::GroundElev, 250.0)));

    readEveryFrameOutput(session);
    QVERIFY(downIs(session, {0.0, -100.0, -200.0, -300.0}));
    QCOMPARE(engine.runCount("builtin.local.coordinates"), 1);

    QVERIFY(!session.hasSensor("Local"));
    QVERIFY(!session.sensorKeys().contains(QStringLiteral("Local")));
    QCOMPARE(engine.undeclaredReadCount(), 0);
}

FLYSIGHT_TEST_MAIN(LocalCoordinatesTest)
#include "tst_local_coordinates.moc"
