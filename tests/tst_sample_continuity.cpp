// tst_sample_continuity: the continuity rule (src/samplecontinuity.h), the one
// authority on when the interval between two samples of a sensor is a hole,
// on hand-built axes; and the plot utilities that read a sensor's samples
// through it (src/plotutils.h): a graph's points with a break across each
// hole, the point reads of the legend, the measure tool and the Set Ground
// tool, and the crosshair's reading of a graph. Nothing here needs a widget:
// PlotWidget builds its graphs with graphData() and reads them with
// interpolateGraphAt().
//
// The session is synthetic and stored: GNSS fixes a second apart with a 5 s
// hole between T0 + 3 and T0 + 8, and IMU samples a second apart without one.
// Every time is stored as _time and _system_time directly (stored data wins
// over any calculation), and the system time is evenly spaced across the GNSS
// hole, so a break that follows it is judged on the sensor's own _time.

#include <QtTest>

#include <cmath>
#include <limits>
#include <vector>

#include "plotutils.h"
#include "samplecontinuity.h"
#include "sessiondata.h"
#include "testenvironment.h"
#include "testmain.h"

using namespace FlySight;
using namespace FlySightTest;
using namespace FlySight::SampleContinuity;

namespace {

constexpr double T0 = 1704110400.0;

const QVector<double> gnssTime = {T0, T0 + 1.0, T0 + 2.0, T0 + 3.0, T0 + 8.0, T0 + 9.0, T0 + 10.0};
const QVector<double> gnssSystemTime = {100.0, 102.0, 104.0, 106.0, 108.0, 110.0, 112.0};
const QVector<double> hMSL = {1000.0, 990.0, 980.0, 970.0, 920.0, 910.0, 900.0};

// A graph point as QCPGraphData has it
struct Point {
    double key;
    double value;
};

SessionData holedSession()
{
    SessionData session;
    session.setSourceMeasurement("GNSS", "_time", gnssTime, "s");
    session.setSourceMeasurement("GNSS", "_system_time", gnssSystemTime, "s");
    session.setSourceMeasurement("GNSS", "hMSL", hMSL, "m");

    QVector<double> imuTime, imuSystemTime, ax;
    for (int i = 0; i <= 10; ++i) {
        imuTime.append(T0 + i);
        imuSystemTime.append(100.0 + 2 * i);
        ax.append(0.5 * i);
    }
    session.setSourceMeasurement("IMU", "_time", imuTime, "s");
    session.setSourceMeasurement("IMU", "_system_time", imuSystemTime, "s");
    session.setSourceMeasurement("IMU", "ax", ax, "g");
    session.setAttribute("_REF", T0);
    return session;
}

std::vector<Point> pointsOf(const GraphData &graph)
{
    std::vector<Point> points;
    for (qsizetype i = 0; i < graph.keys.size(); ++i)
        points.push_back({graph.keys[i], graph.values[i]});
    return points;
}

// The indices of the NaN-valued points
QList<qsizetype> breaksOf(const GraphData &graph)
{
    QList<qsizetype> breaks;
    for (qsizetype i = 0; i < graph.values.size(); ++i) {
        if (std::isnan(graph.values[i]))
            breaks.append(i);
    }
    return breaks;
}

} // namespace

class SampleContinuityTest : public QObject {
    Q_OBJECT

private slots:
    void nominalIntervalIsTheMedian();
    void holeIsStrictlyAboveOneAndAHalf();
    void shortAxisHasNoHoles();
    void malformedAxisHasNoHoles();
    void runsPartitionTheSamples();

    void graphBreaksAtGnssHole();
    void imuGraphHasNoBreak();
    void sessionReadInsideHole();
    void measureEndReadInsideHole();
    void groundElevationInsideHole();
    void crosshairReadInsideHole();
};

// The median of the successive differences: the middle one for an odd count,
// the mean of the two middle ones for an even count; QVector and std::vector
// are read alike, in place
void SampleContinuityTest::nominalIntervalIsTheMedian()
{
    const QVector<double> odd = {0.0, 1.0, 3.0, 4.5};                 // 1, 2, 1.5
    QCOMPARE(nominalInterval(odd), 1.5);
    const std::vector<double> even = {0.0, 1.0, 3.0, 6.0, 10.0};      // 1, 2, 3, 4
    QCOMPARE(nominalInterval(even), 2.5);
    QCOMPARE(holeThreshold(even), 3.75);
    QCOMPARE(nominalInterval(gnssTime), 1.0);
    QCOMPARE(holeThreshold(gnssTime), 1.5);
    const double raw[] = {10.0, 10.25, 10.5, 11.0};                   // .25, .25, .5
    QCOMPARE(nominalInterval(TimeAxis(raw, 4)), .25);
}

// An interval of exactly 1.5 nominal intervals is not a hole; the next double
// above it is
void SampleContinuityTest::holeIsStrictlyAboveOneAndAHalf()
{
    const double above = std::nextafter(1.5, 2.0);
    QVERIFY(!isHole(1.5, 1.5));
    QVERIFY(isHole(above, 1.5));
    QVERIFY(!isHole(1.0, 1.5));

    const std::vector<double> boundary = {0.0, 1.0, 2.0, 3.0, 4.5, 5.5, 6.5};
    QCOMPARE(holeThreshold(boundary), 1.5);
    QVERIFY(!isHoleBefore(boundary, 4, holeThreshold(boundary)));
    QCOMPARE(runs(boundary).size(), size_t(1));

    // The next double above 4.5 is 1.5 plus one of its ulps after 3: an
    // interval just above 1.5, the others exactly 1
    const double x = std::nextafter(4.5, 5.0);
    const std::vector<double> justAbove = {0.0, 1.0, 2.0, 3.0, x, x + 1.0, x + 2.0};
    QVERIFY(justAbove[4] - justAbove[3] > 1.5);
    QCOMPARE(holeThreshold(justAbove), 1.5);
    QVERIFY(isHoleBefore(justAbove, 4, holeThreshold(justAbove)));
    for (std::size_t i : {std::size_t(0), std::size_t(1), std::size_t(3), std::size_t(5), std::size_t(7)})
        QVERIFY(!isHoleBefore(justAbove, i, holeThreshold(justAbove)));

    // Never a hole against no threshold
    QVERIFY(!isHole(1e9, std::numeric_limits<double>::quiet_NaN()));
}

// Fewer than three samples: no nominal interval, no holes, one run (none
// when empty)
void SampleContinuityTest::shortAxisHasNoHoles()
{
    const std::vector<double> two = {0.0, 100.0};
    QVERIFY(std::isnan(nominalInterval(two)));
    QVERIFY(std::isnan(holeThreshold(two)));
    QVERIFY(!isHoleBefore(two, 1, holeThreshold(two)));
    QCOMPARE(runs(two).size(), size_t(1));
    QCOMPARE(runs(two)[0].begin, size_t(0));
    QCOMPARE(runs(two)[0].end, size_t(2));

    const std::vector<double> one = {5.0};
    QVERIFY(std::isnan(nominalInterval(one)));
    QCOMPARE(runs(one).size(), size_t(1));
    QVERIFY(runs(std::vector<double>()).empty());
}

// An axis that is not finite and strictly increasing (a session does not
// exclude one) has no nominal interval and no holes, and nothing throws
void SampleContinuityTest::malformedAxisHasNoHoles()
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const QList<std::vector<double>> axes = {
        {0.0, 2.0, 1.0, 3.0, 20.0},
        {0.0, 1.0, 1.0, 2.0, 20.0},
        {0.0, 1.0, nan, 3.0, 20.0},
        {0.0, 1.0, 2.0, inf},
    };
    for (const std::vector<double> &axis : axes) {
        QVERIFY(std::isnan(nominalInterval(axis)));
        QVERIFY(std::isnan(holeThreshold(axis)));
        const std::vector<Run> r = runs(axis);
        QCOMPARE(r.size(), size_t(1));
        QCOMPARE(r[0].end, axis.size());
    }
}

// Two holes make three half-open runs that partition the samples in order
void SampleContinuityTest::runsPartitionTheSamples()
{
    const std::vector<double> axis = {0.0, 1.0, 2.0, 10.0, 11.0, 12.0, 20.0, 21.0};
    const std::vector<Run> r = runs(axis);
    QCOMPARE(r.size(), size_t(3));
    QCOMPARE(r[0].begin, size_t(0));
    QCOMPARE(r[0].end, size_t(3));
    QCOMPARE(r[1].begin, size_t(3));
    QCOMPARE(r[1].end, size_t(6));
    QCOMPARE(r[2].begin, size_t(6));
    QCOMPARE(r[2].end, size_t(8));
}

// A GNSS series has exactly one break, a NaN-valued point keyed strictly
// between the two samples around the hole, and every other point is a
// sample in order; against _system_time, whose spacing has no hole, the
// break is at the same sample pair
void SampleContinuityTest::graphBreaksAtGnssHole()
{
    const SessionData session = holedSession();

    struct Variant {
        QString xVariable;
        QVector<double> x;
        double offset;
    };
    const Variant variants[] = {
        {QStringLiteral("_time"), gnssTime, 0.0},
        {QStringLiteral("_time"), gnssTime, T0},
        {QStringLiteral("_system_time"), gnssSystemTime, 0.0},
    };
    for (const Variant &v : variants) {
        const GraphData graph = graphData(session, "GNSS", "hMSL", v.xVariable, v.offset);
        QCOMPARE(graph.keys.size(), hMSL.size() + 1);
        QCOMPARE(graph.values.size(), hMSL.size() + 1);
        QCOMPARE(breaksOf(graph), QList<qsizetype>({4}));
        QVERIFY(graph.keys[4] > graph.keys[3]);
        QVERIFY(graph.keys[4] < graph.keys[5]);
        for (qsizetype i = 0; i < hMSL.size(); ++i) {
            const qsizetype point = i < 4 ? i : i + 1;
            QCOMPARE(graph.keys[point], v.x[i] - v.offset);
            QCOMPARE(graph.values[point], hMSL[i]);
        }
    }

    // A measurement the session does not have: no graph
    QVERIFY(graphData(session, "GNSS", "velD", "_time", 0.0).keys.isEmpty());
}

// The IMU of the same session has no hole: its graph is its samples
void SampleContinuityTest::imuGraphHasNoBreak()
{
    const SessionData session = holedSession();
    for (const char *x : {"_time", "_system_time"}) {
        const GraphData graph = graphData(session, "IMU", "ax", x, 0.0);
        QCOMPARE(graph.keys, session.getMeasurement("IMU", x));
        QCOMPARE(graph.values, session.getMeasurement("IMU", "ax"));
        QVERIFY(breaksOf(graph).isEmpty());
    }
}

// The legend's read: nothing strictly inside the hole, the sample's value at
// the samples around it, as before between connected samples and beyond the
// ends; against _system_time the hole is still the GNSS samples' own
void SampleContinuityTest::sessionReadInsideHole()
{
    const SessionData session = holedSession();
    const SensorTimeAxis gnss = sensorTimeAxis(session, "GNSS");
    QCOMPARE(gnss.time, gnssTime);
    QCOMPARE(gnss.holeThreshold, 1.5);

    const auto read = [&](const QString &x, double at) {
        return interpolateSessionMeasurement(session, "GNSS", x, "hMSL", at, gnss);
    };
    const QString utc = QStringLiteral("_time"), system = QStringLiteral("_system_time");

    QVERIFY(std::isnan(read(utc, T0 + 5.0)));
    QVERIFY(std::isnan(read(utc, T0 + 3.25)));
    QVERIFY(std::isnan(read(utc, T0 + 7.75)));
    QCOMPARE(read(utc, T0 + 3.0), 970.0);
    QCOMPARE(read(utc, T0 + 8.0), 920.0);
    QCOMPARE(read(utc, T0 + 1.5), 985.0);
    QCOMPARE(read(utc, T0 + 9.5), 905.0);
    QVERIFY(std::isnan(read(utc, T0 - 1.0)));
    QVERIFY(std::isnan(read(utc, T0)));             // the first sample, as before
    QVERIFY(std::isnan(read(utc, T0 + 11.0)));

    QVERIFY(std::isnan(read(system, 107.0)));
    QCOMPARE(read(system, 106.0), 970.0);
    QCOMPARE(read(system, 108.0), 920.0);
    QCOMPARE(read(system, 103.0), 985.0);

    // A batch of reads asks for each sensor's axis once
    SensorTimeAxes axes;
    const SensorTimeAxis &first = axes.of(session, "GNSS");
    QCOMPARE(&axes.of(session, "GNSS"), &first);
    QCOMPARE(first.holeThreshold, 1.5);

    // A sensor whose _time is not the data's length has no holes here
    SensorTimeAxis unjudged;
    unjudged.time = {T0, T0 + 1.0};
    unjudged.holeThreshold = holeThreshold(unjudged.time);
    QCOMPARE(interpolateSessionMeasurement(session, "GNSS", utc, "hMSL", T0 + 5.0, unjudged), 950.0);
}

// The measure tool's end read (interpolateAtX on the plot's x data): the
// same answers, also on _system_time keys
void SampleContinuityTest::measureEndReadInsideHole()
{
    const SessionData session = holedSession();
    const SensorTimeAxis gnss = sensorTimeAxis(session, "GNSS");
    for (const QVector<double> &x : {gnssTime, gnssSystemTime}) {
        const double before = x[3], after = x[4];
        QVERIFY(std::isnan(interpolateAtX(x, hMSL, gnss, (before + after) / 2)));
        QCOMPARE(interpolateAtX(x, hMSL, gnss, before), 970.0);
        QCOMPARE(interpolateAtX(x, hMSL, gnss, after), 920.0);
        QCOMPARE(interpolateAtX(x, hMSL, gnss, (x[1] + x[2]) / 2), 985.0);
        QVERIFY(std::isnan(interpolateAtX(x, hMSL, gnss, x.last() + 1.0)));
    }
}

// The Set Ground tool's elevation at a click: GNSS/hMSL at that time, none
// inside the hole, the sample's at the samples around it, none outside
void SampleContinuityTest::groundElevationInsideHole()
{
    const SessionData session = holedSession();
    const QString utc = QStringLiteral("_time");
    QVERIFY(std::isnan(groundElevationAt(session, utc, QString(), T0 + 5.0)));
    QCOMPARE(groundElevationAt(session, utc, QString(), T0 + 3.0), 970.0);
    QCOMPARE(groundElevationAt(session, utc, QString(), T0 + 8.0), 920.0);
    QCOMPARE(groundElevationAt(session, utc, QString(), T0 + 1.5), 985.0);
    QVERIFY(std::isnan(groundElevationAt(session, utc, QString(), T0 - 1.0)));
    QVERIFY(std::isnan(groundElevationAt(session, utc, QString(), T0 + 11.0)));

    // Relative to a reference marker at T0
    QVERIFY(std::isnan(groundElevationAt(session, utc, QStringLiteral("_REF"), 5.0)));
    QCOMPARE(groundElevationAt(session, utc, QStringLiteral("_REF"), 8.0), 920.0);
}

// The crosshair's reading of the graph: NaN on both sides of the break, the
// sample's own value at the sample before the hole and at the one after it
// (whose neighbour is the break), as before elsewhere
void SampleContinuityTest::crosshairReadInsideHole()
{
    const SessionData session = holedSession();
    const std::vector<Point> points = pointsOf(graphData(session, "GNSS", "hMSL", "_time", T0));
    const auto read = [&points](double x) { return interpolateGraphAt(points.cbegin(), points.cend(), x); };

    QVERIFY(std::isnan(read(4.0)));       // before the break
    QVERIFY(std::isnan(read(5.5)));       // at the break
    QVERIFY(std::isnan(read(7.0)));       // after the break
    QVERIFY(std::isnan(read(7.999)));
    QCOMPARE(read(3.0), 970.0);
    QCOMPARE(read(8.0), 920.0);
    QCOMPARE(read(1.5), 985.0);
    QCOMPARE(read(8.5), 915.0);
    QVERIFY(std::isnan(read(-1.0)));
    QVERIFY(std::isnan(read(0.0)));       // the first point, as before
    QVERIFY(std::isnan(read(10.5)));
}

FLYSIGHT_TEST_MAIN(SampleContinuityTest)
#include "tst_sample_continuity.moc"
