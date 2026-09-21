// The simplified map track (builtin.simplified.track) on the shared local
// frame: seven outputs at the same retained sample indices, the 0.5 m
// tolerance, duplicate-position endpoints, closed / degenerate / empty tracks,
// non-finite samples, the single projection, and unavailability without a
// local origin with recovery. Everything runs against a fake session state and
// a private registry. Index selection is isolated from the projection by
// storing Local/north, east, down directly (stored data wins over any
// calculation), and GNSS/time is the sample index, so Simplified/_time reads
// back as the retained indices and every expectation is a literal.

#include <algorithm>
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
#include "testmain.h"

using namespace FlySight;
using namespace FlySightTest;
using Synthetic::measKey;

namespace {

constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
constexpr double Inf = std::numeric_limits<double>::infinity();

const char TrackId[] = "builtin.simplified.track";
const char FrameId[] = "builtin.local.coordinates";

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

// Each output with the input it is taken from.
struct Channel {
    const char *name;
    const char *inputSensor;
    const char *inputName;
};

const Channel channels[] = {
    {"lat", "GNSS", "lat"},
    {"lon", "GNSS", "lon"},
    {"hMSL", "GNSS", "hMSL"},
    {"_time", "GNSS", "_time"},
    {"north", "Local", "north"},
    {"east", "Local", "east"},
    {"down", "Local", "down"},
};

// A path given directly in the local frame. GNSS/time is the sample index, and
// the geographic channels are distinct at every sample, so a wrong index shows
// in every output.
void addPath(FakeSessionState &state, const QVector<double> &north, const QVector<double> &east,
             QVector<double> down = {})
{
    const int n = north.size();
    QVector<double> time, lat, lon, hMSL;
    for (int i = 0; i < n; ++i) {
        time.append(i);
        lat.append(45.0 + i * 0.001);
        lon.append(-75.0 + i * 0.001);
        hMSL.append(1000.0 + i);
        if (down.size() < n)
            down.append(-double(i));
    }

    state.setMeasurement("GNSS", "time", time);
    state.setMeasurement("GNSS", "lat", lat);
    state.setMeasurement("GNSS", "lon", lon);
    state.setMeasurement("GNSS", "hMSL", hMSL);
    state.setMeasurement("Local", "north", north);
    state.setMeasurement("Local", "east", east);
    state.setMeasurement("Local", "down", down);
}

// The real path: a GNSS track and nothing under Local. The local-coordinates
// calculation needs the velocities, which are zero.
void addGnssTrack(FakeSessionState &state, const QVector<double> &lat, const QVector<double> &lon,
                  const QVector<double> &hMSL, const QVector<double> &hAcc)
{
    const int n = lat.size();
    QVector<double> time;
    for (int i = 0; i < n; ++i)
        time.append(i);

    state.setMeasurement("GNSS", "time", time);
    state.setMeasurement("GNSS", "lat", lat);
    state.setMeasurement("GNSS", "lon", lon);
    state.setMeasurement("GNSS", "hMSL", hMSL);
    state.setMeasurement("GNSS", "hAcc", hAcc);
    state.setMeasurement("GNSS", "velN", QVector<double>(n, 0.0));
    state.setMeasurement("GNSS", "velE", QVector<double>(n, 0.0));
    state.setMeasurement("GNSS", "velD", QVector<double>(n, 0.0));
}

// Ten fixes about 11 m apart along one meridian, except fix 5, which is about
// 7.9 m to the east.
void addDogleg(FakeSessionState &state, double hAcc)
{
    QVector<double> lat;
    for (int i = 0; i < 10; ++i)
        lat.append(45.0 + i * 1e-4);
    QVector<double> lon(10, -75.0);
    lon[5] = -75.0 + 1e-4;
    addGnssTrack(state, lat, lon, QVector<double>(10, 1000.0), QVector<double>(10, hAcc));
}

// The seven outputs have one length, Simplified/_time (the retained indices)
// is strictly increasing, and every output is exactly its input at those
// indices. Returns a description of the first difference, or nothing.
QString alignmentDifference(CalculationEngine &engine)
{
    const QVector<double> idx = engine.measurement("Simplified", "_time");
    for (int k = 1; k < idx.size(); ++k) {
        if (!(idx[k] > idx[k - 1]))
            return QStringLiteral("indices not strictly increasing at %1").arg(k);
    }

    for (const Channel &channel : channels) {
        const QVector<double> output = engine.measurement("Simplified", channel.name);
        const QVector<double> input = engine.measurement(channel.inputSensor, channel.inputName);
        if (output.size() != idx.size())
            return QStringLiteral("%1 has %2 samples, _time has %3")
                .arg(channel.name).arg(output.size()).arg(idx.size());
        for (int k = 0; k < idx.size(); ++k) {
            const int i = int(idx[k]);
            if (i < 0 || i >= input.size() || double(i) != idx[k])
                return QStringLiteral("%1 is not a sample index").arg(idx[k]);
            if (!(output[k] == input[i]))
                return QStringLiteral("%1[%2] is not sample %3").arg(channel.name).arg(k).arg(i);
        }
    }
    return QString();
}

bool allFinite(CalculationEngine &engine)
{
    for (const Channel &channel : channels) {
        for (double value : engine.measurement("Simplified", channel.name)) {
            if (!std::isfinite(value))
                return false;
        }
    }
    return true;
}

bool allEmpty(CalculationEngine &engine)
{
    for (const Channel &channel : channels) {
        if (!engine.measurement("Simplified", channel.name).isEmpty())
            return false;
    }
    return true;
}

bool containsEveryOutput(const QSet<DependencyKey> &keys)
{
    for (const Channel &channel : channels) {
        if (!keys.contains(measKey("Simplified", channel.name)))
            return false;
    }
    return true;
}

// Horizontal distance from sample i to the segment a-b, written independently
// of the code under test.
double distanceToSegment(const QVector<double> &north, const QVector<double> &east, int a, int b, int i)
{
    const double sn = north[b] - north[a];
    const double se = east[b] - east[a];
    const double length2 = sn * sn + se * se;
    double t = 0.0;
    if (length2 > 0.0)
        t = std::min(1.0, std::max(0.0, ((north[i] - north[a]) * sn + (east[i] - east[a]) * se) / length2));
    return std::hypot(north[i] - (north[a] + t * sn), east[i] - (east[a] + t * se));
}

// 1000 samples, 0.1 m apart to the north, weaving 5 m either side.
void wavePath(QVector<double> &north, QVector<double> &east)
{
    for (int i = 0; i < 1000; ++i) {
        north.append(0.1 * i);
        east.append(5.0 * std::sin(0.02 * i));
    }
}

QVector<double> retained(CalculationEngine &engine)
{
    return engine.measurement("Simplified", "_time");
}

} // namespace

class SimplifiedTrackTest : public QObject {
    Q_OBJECT

private slots:
    void sevenOutputsShareIndices();
    void droppedSamplesWithinTolerance();
    void duplicatePositionEndpoints();
    void closedTrack();
    void degenerateTracks_data();
    void degenerateTracks();
    void emptyTrack_data();
    void emptyTrack();
    void nonFiniteSamplesAreSkipped_data();
    void nonFiniteSamplesAreSkipped();
    void mismatchedLengthsUnavailable();
    void projectionRunsOncePerRecording();
    void unavailableWithoutOriginAndRecovers();
    void siblingsInvalidateTogether();
};

// One calculation behind all seven names, and every one of them is the
// recorded sample at the same indices.
void SimplifiedTrackTest::sevenOutputsShareIndices()
{
    World world;
    QVector<double> north, east;
    wavePath(north, east);
    addPath(world.state, north, east);
    CalculationEngine &engine = *world.engine;

    for (int pass = 0; pass < 2; ++pass) {
        for (const char *name : {"down", "lat", "_time", "east", "hMSL", "north", "lon"})
            QVERIFY2(!engine.measurement("Simplified", name).isEmpty(), name);
    }
    QCOMPARE(engine.runCount(TrackId), 1);

    const QString difference = alignmentDifference(engine);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));

    const int size = retained(engine).size();
    QVERIFY2(size > 2 && size < 100, qPrintable(QString::number(size)));

    // Stored Local data won: nothing was projected
    QCOMPARE(engine.runCount(FrameId), 0);
    QCOMPARE(engine.runCount(TrackId), 1);
    QCOMPARE(engine.undeclaredReadCount(), 0);
}

// Every sample, dropped or not, lies within 0.5 m of the simplified path, and
// a sample survives only when it is strictly further than that.
void SimplifiedTrackTest::droppedSamplesWithinTolerance()
{
    {
        World world;
        QVector<double> north, east;
        wavePath(north, east);
        addPath(world.state, north, east);
        CalculationEngine &engine = *world.engine;

        const QVector<double> idx = retained(engine);
        QVERIFY(idx.size() > 2);
        QCOMPARE(idx.first(), 0.0);
        QCOMPARE(idx.last(), 999.0);
        for (int k = 1; k < idx.size(); ++k) {
            const int a = int(idx[k - 1]);
            const int b = int(idx[k]);
            for (int i = a; i <= b; ++i) {
                const double distance = distanceToSegment(north, east, a, b, i);
                QVERIFY2(distance <= 0.5 + 1e-9,
                         qPrintable(QStringLiteral("sample %1: %2 m").arg(i).arg(distance)));
            }
        }
        QCOMPARE(engine.undeclaredReadCount(), 0);
    }
    {
        World world;
        addPath(world.state, {0, 1, 2}, {0, 0.49, 0});
        QCOMPARE(retained(*world.engine), QVector<double>({0, 2}));
        QCOMPARE(world.engine->undeclaredReadCount(), 0);
    }
    {
        World world;
        addPath(world.state, {0, 1, 2}, {0, 0.51, 0});
        QCOMPARE(retained(*world.engine), QVector<double>({0, 1, 2}));
        QCOMPARE(world.engine->undeclaredReadCount(), 0);
    }
}

// Distinct samples at one position stay distinct. The previous implementation
// simplified points and then searched the recording for each of them; here the
// search for the second point started at, and matched, the first, so sample 0
// came back twice and the true last sample was lost. Retaining indices removes
// that defect.
void SimplifiedTrackTest::duplicatePositionEndpoints()
{
    {
        World world;
        addPath(world.state, {0, 0, 0, 0}, {0, 0, 0, 0});
        CalculationEngine &engine = *world.engine;

        QCOMPARE(retained(engine), QVector<double>({0, 3}));
        QCOMPARE(engine.measurement("Simplified", "hMSL"), QVector<double>({1000.0, 1003.0}));
        QCOMPARE(engine.measurement("Simplified", "down"), QVector<double>({0.0, -3.0}));
        QCOMPARE(engine.measurement("Simplified", "lat"), QVector<double>({45.0, 45.003}));
        QCOMPARE(engine.undeclaredReadCount(), 0);
    }
    {
        // A straight path that ends stationary: the end is the true last sample
        World world;
        addPath(world.state, {0, 10, 20, 20}, {0, 0, 0, 0});
        CalculationEngine &engine = *world.engine;

        QCOMPARE(retained(engine), QVector<double>({0, 3}));
        QCOMPARE(engine.measurement("Simplified", "hMSL"), QVector<double>({1000.0, 1003.0}));
        QCOMPARE(engine.undeclaredReadCount(), 0);
    }
}

// With coincident endpoints the distance is to that point, so an excursion
// survives.
void SimplifiedTrackTest::closedTrack()
{
    {
        World world;
        addPath(world.state, {0, 3, 0}, {0, 0, 0});
        QCOMPARE(retained(*world.engine), QVector<double>({0, 1, 2}));
        QCOMPARE(world.engine->undeclaredReadCount(), 0);
    }
    {
        // A square returning to its start
        World world;
        addPath(world.state, {0, 10, 10, 0, 0}, {0, 0, 10, 10, 0});
        CalculationEngine &engine = *world.engine;

        QCOMPARE(retained(engine), QVector<double>({0, 1, 2, 3, 4}));
        const QString difference = alignmentDifference(engine);
        QVERIFY2(difference.isEmpty(), qPrintable(difference));
        QCOMPARE(engine.undeclaredReadCount(), 0);
    }
}

void SimplifiedTrackTest::degenerateTracks_data()
{
    QTest::addColumn<QVector<double>>("north");
    QTest::addColumn<QVector<double>>("east");
    QTest::addColumn<QVector<double>>("expected");

    QTest::newRow("one sample")
        << QVector<double>({7}) << QVector<double>({3}) << QVector<double>({0});
    QTest::newRow("two samples 100 m apart")
        << QVector<double>({0, 100}) << QVector<double>({0, 0}) << QVector<double>({0, 1});
    QTest::newRow("two coincident samples")
        << QVector<double>({5, 5}) << QVector<double>({5, 5}) << QVector<double>({0, 1});
    QTest::newRow("five collinear samples")
        << QVector<double>({0, 10, 20, 30, 40}) << QVector<double>({0, 5, 10, 15, 20})
        << QVector<double>({0, 4});
    QTest::newRow("five samples within 0.3 m")
        << QVector<double>({0, 0.1, 0.2, 0.1, 0.2}) << QVector<double>({0, 0.1, 0, 0.2, 0.1})
        << QVector<double>({0, 4});
}

void SimplifiedTrackTest::degenerateTracks()
{
    QFETCH(QVector<double>, north);
    QFETCH(QVector<double>, east);
    QFETCH(QVector<double>, expected);

    World world;
    addPath(world.state, north, east);
    CalculationEngine &engine = *world.engine;

    QCOMPARE(retained(engine), expected);
    for (const Channel &channel : channels)
        QVERIFY2(engine.measurement("Simplified", channel.name).size() == expected.size(), channel.name);
    QVERIFY(engine.resultStatus(TrackId) == ResultStatus::Ok);

    const QString difference = alignmentDifference(engine);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    QCOMPARE(engine.undeclaredReadCount(), 0);
}

void SimplifiedTrackTest::emptyTrack_data()
{
    QTest::addColumn<bool>("emptyColumns");
    QTest::newRow("no GNSS data") << false;
    QTest::newRow("zero-length columns") << true;
}

// Nothing to simplify is a missing input: nothing runs.
void SimplifiedTrackTest::emptyTrack()
{
    QFETCH(bool, emptyColumns);

    World world;
    if (emptyColumns)
        addPath(world.state, {}, {});
    CalculationEngine &engine = *world.engine;

    QVERIFY(allEmpty(engine));
    QVERIFY(engine.resultStatus(TrackId) == ResultStatus::MissingInput);
    QCOMPARE(engine.runCount(TrackId), 0);
    QCOMPARE(engine.undeclaredReadCount(), 0);
}

void SimplifiedTrackTest::nonFiniteSamplesAreSkipped_data()
{
    QTest::addColumn<QVector<double>>("north");
    QTest::addColumn<QVector<double>>("east");
    QTest::addColumn<QVector<double>>("down");
    QTest::addColumn<QVector<double>>("expected");    // empty: unavailable

    const QVector<double> line = {0, 1, 2, 3, 4};
    const QVector<double> zeros(5, 0.0);
    const QVector<double> level = {0, -1, -2, -3, -4};

    // The hole is bridged; its neighbour is judged against the segment 0-4
    QTest::newRow("hole next to a corner")
        << line << QVector<double>({0, NaN, 0.51, 0, 0}) << level << QVector<double>({0, 2, 4});
    // The endpoints are the first and last finite samples
    QTest::newRow("non-finite ends")
        << QVector<double>({NaN, 1, 2, 3, NaN}) << zeros << level << QVector<double>({1, 3});
    // Any of the three local coordinates disqualifies the sample
    QTest::newRow("corner without down")
        << line << QVector<double>({0, 0, 0.51, 0, 0}) << QVector<double>({0, -1, NaN, -3, -4})
        << QVector<double>({0, 4});
    QTest::newRow("infinities")
        << line << QVector<double>({0, Inf, 0, -Inf, 0}) << level << QVector<double>({0, 4});
    QTest::newRow("nothing finite")
        << QVector<double>(5, NaN) << zeros << level << QVector<double>();
}

void SimplifiedTrackTest::nonFiniteSamplesAreSkipped()
{
    QFETCH(QVector<double>, north);
    QFETCH(QVector<double>, east);
    QFETCH(QVector<double>, down);
    QFETCH(QVector<double>, expected);

    World world;
    addPath(world.state, north, east, down);
    CalculationEngine &engine = *world.engine;

    QCOMPARE(retained(engine), expected);
    QVERIFY(engine.resultStatus(TrackId) == ResultStatus::Ok);
    QCOMPARE(engine.runCount(TrackId), 1);

    if (expected.isEmpty()) {
        // The calculation ran and produced nothing
        QVERIFY(allEmpty(engine));
        for (const Channel &channel : channels) {
            QVERIFY2(engine.cachedState(measKey("Simplified", channel.name))
                         == CalculationEngine::CachedState::Unavailable, channel.name);
        }
    } else {
        QVERIFY(allFinite(engine));
        const QString difference = alignmentDifference(engine);
        QVERIFY2(difference.isEmpty(), qPrintable(difference));
    }
    QCOMPARE(engine.runCount(TrackId), 1);
    QCOMPARE(engine.undeclaredReadCount(), 0);
}

// Ragged channels have no common sample index.
void SimplifiedTrackTest::mismatchedLengthsUnavailable()
{
    World world;
    addPath(world.state, {0, 1, 2, 3, 4}, {0, 0, 0, 0});
    CalculationEngine &engine = *world.engine;

    QVERIFY(allEmpty(engine));
    QVERIFY(engine.resultStatus(TrackId) == ResultStatus::Ok);
    QCOMPARE(engine.runCount(TrackId), 1);
    QCOMPARE(engine.undeclaredReadCount(), 0);
}

// The real path. The local-coordinates calculation projects the recording
// once, whatever is read and in whatever order, and the simplified local
// outputs are exactly its samples. (That nothing else in the application
// constructs a projection is a rule of the cleanup audit.)
//
// Expected indices, by hand: fix 5 is 7.9 m off the chord 0-9. Fixes 1-4 lie
// on the meridian, up to 6.3 m off the chord 0-5, and are collinear among
// themselves, so only 4 survives; 6-8 mirror that.
void SimplifiedTrackTest::projectionRunsOncePerRecording()
{
    World world;
    addDogleg(world.state, 1.0);
    CalculationEngine &engine = *world.engine;

    for (int pass = 0; pass < 2; ++pass) {
        QVERIFY(!engine.measurement("Simplified", "east").isEmpty());
        QVERIFY(!engine.measurement("Local", "velD").isEmpty());
        QVERIFY(engine.attribute("_LOCAL_ORIGIN_LON").isValid());
        QVERIFY(!engine.measurement("Simplified", "lat").isEmpty());
        QVERIFY(!engine.measurement("Local", "north").isEmpty());
        QVERIFY(!engine.measurement("Simplified", "down").isEmpty());
        QVERIFY(engine.attribute("_LOCAL_ORIGIN_INDEX").isValid());
        QVERIFY(!engine.measurement("Local", "velE").isEmpty());
        QVERIFY(!engine.measurement("Simplified", "_time").isEmpty());
        QVERIFY(!engine.measurement("Local", "down").isEmpty());
        QVERIFY(!engine.measurement("Simplified", "hMSL").isEmpty());
        QVERIFY(engine.attribute("_LOCAL_ORIGIN_LAT").isValid());
        QVERIFY(!engine.measurement("Local", "east").isEmpty());
        QVERIFY(!engine.measurement("Simplified", "north").isEmpty());
        QVERIFY(!engine.measurement("Local", "velN").isEmpty());
        QVERIFY(engine.attribute("_LOCAL_ORIGIN_HMSL").isValid());
        QVERIFY(!engine.measurement("Simplified", "lon").isEmpty());
    }
    QCOMPARE(engine.runCount(FrameId), 1);
    QCOMPARE(engine.runCount(TrackId), 1);

    QCOMPARE(retained(engine), QVector<double>({0, 4, 5, 6, 9}));
    const QString difference = alignmentDifference(engine);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));

    QCOMPARE(engine.runCount(FrameId), 1);
    QCOMPARE(engine.runCount(TrackId), 1);
    QCOMPARE(engine.undeclaredReadCount(), 0);
    QCOMPARE(engine.cycleCount(), 0);
}

// No qualifying origin: no shared frame, so no track (a missing input, not a
// fallback), and the track returns when the source is corrected.
void SimplifiedTrackTest::unavailableWithoutOriginAndRecovers()
{
    World world;
    addDogleg(world.state, 10.0);
    CalculationEngine &engine = *world.engine;

    QVERIFY(allEmpty(engine));
    QVERIFY(engine.resultStatus(TrackId) == ResultStatus::MissingInput);
    QCOMPARE(engine.runCount(TrackId), 0);
    QVERIFY(engine.cachedState(measKey("Simplified", "lat")) == CalculationEngine::CachedState::Unavailable);

    // Corrected: all seven names are invalidated and become available
    QSet<DependencyKey> invalidated =
        world.state.setMeasurement(engine, "GNSS", "hAcc", QVector<double>(10, 1.0));
    QVERIFY(containsEveryOutput(invalidated));
    QCOMPARE(retained(engine), QVector<double>({0, 4, 5, 6, 9}));
    QString difference = alignmentDifference(engine);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    QCOMPARE(engine.runCount(TrackId), 1);

    // Lost again: no stale geometry
    invalidated = world.state.setMeasurement(engine, "GNSS", "hAcc", QVector<double>(10, 10.0));
    QVERIFY(containsEveryOutput(invalidated));
    QVERIFY(allEmpty(engine));
    QVERIFY(engine.resultStatus(TrackId) == ResultStatus::MissingInput);
    QCOMPARE(engine.runCount(TrackId), 1);

    // A fix without a position never reaches the track. Here it is the corner,
    // so what remains is a straight line.
    world.state.setMeasurement(engine, "GNSS", "hAcc", QVector<double>(10, 1.0));
    QVector<double> lat = engine.measurement("GNSS", "lat");
    QCOMPARE(lat.size(), 10);
    lat[5] = NaN;
    invalidated = world.state.setMeasurement(engine, "GNSS", "lat", lat);
    QVERIFY(invalidated.contains(measKey("GNSS", "lat")));
    QVERIFY(!retained(engine).contains(5.0));
    QCOMPARE(retained(engine), QVector<double>({0, 9}));
    QVERIFY(allFinite(engine));
    difference = alignmentDifference(engine);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));

    QCOMPARE(engine.undeclaredReadCount(), 0);
    QCOMPARE(engine.cycleCount(), 0);
}

// The seven outputs are one result: a change of one input drops all of them.
// The engine reports the names that had been resolved (a name nobody has read
// has no reader to tell), so three of them are read first.
void SimplifiedTrackTest::siblingsInvalidateTogether()
{
    World world;
    addPath(world.state, {0, 1, 2}, {0, 0.51, 0});
    CalculationEngine &engine = *world.engine;

    QCOMPARE(engine.measurement("Simplified", "lat").size(), 3);
    QCOMPARE(engine.measurement("Simplified", "hMSL").size(), 3);
    QCOMPARE(engine.measurement("Simplified", "down").size(), 3);
    QCOMPARE(engine.runCount(TrackId), 1);

    const QSet<DependencyKey> invalidated =
        world.state.setMeasurement(engine, "Local", "east", {0, 0, 0});
    QVERIFY(invalidated.contains(measKey("Simplified", "lat")));
    QVERIFY(invalidated.contains(measKey("Simplified", "hMSL")));
    QVERIFY(invalidated.contains(measKey("Simplified", "down")));
    QVERIFY(!engine.resultStatus(TrackId).has_value());
    QVERIFY(engine.cachedState(measKey("Simplified", "north")) == CalculationEngine::CachedState::NotCached);

    QCOMPARE(engine.measurement("Simplified", "hMSL").size(), 2);
    QCOMPARE(engine.measurement("Simplified", "north"), QVector<double>({0.0, 2.0}));
    QCOMPARE(engine.runCount(TrackId), 2);
    QCOMPARE(engine.undeclaredReadCount(), 0);
}

FLYSIGHT_TEST_MAIN(SimplifiedTrackTest)
#include "tst_simplified_track.moc"
