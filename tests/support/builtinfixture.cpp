#include "builtinfixture.h"

#include <cmath>

#include "dataimporter.h"
#include "engine/calctypes.h"
#include "testenvironment.h"

using namespace FlySight;

namespace FlySightTest {

namespace DescentFixture {

namespace {

Fs2FileBuilder withStandardVars(const QByteArray &sessionId)
{
    Fs2FileBuilder b;
    b.var("FIRMWARE_VER", "v2023.09.22")
     .var("SESSION_ID", sessionId)
     .var("DEVICE_ID", "test-device");
    return b;
}

// Vertical speed profile of row i, in m/s (integers only).
int velDAt(int i)
{
    if (i <= 9)   return 0;
    if (i <= 19)  return 5 * (i - 9);
    if (i <= 73)  return 50;
    if (i <= 265) return (i >= 200 && i <= 204) ? -2 : 5;
    return 0;
}

int velNAt(int i)
{
    if (i <= 73)  return 50;
    if (i <= 265) return 10;
    return 1;
}

SessionData importOne(const Fs2FileBuilder &file, const QString &stem)
{
    const QString path = TestEnvironment::instance().newTempDir(stem)
                         + QLatin1Char('/') + stem + QStringLiteral(".csv");
    SessionData session;
    if (!file.write(path))
        qFatal("DescentFixture: could not write %s", qPrintable(path));
    DataImporter importer;
    if (!importer.importFile(path, session))
        qFatal("DescentFixture: could not import %s: %s", qPrintable(path),
               qPrintable(importer.getLastError()));
    return session;
}

} // namespace

Fs2FileBuilder trackFile(const QByteArray &sessionId)
{
    Fs2FileBuilder b = withStandardVars(sessionId);
    b.sensor("GNSS",
             {"time", "lat", "lon", "hMSL", "velN", "velE", "velD", "hAcc", "vAcc", "sAcc", "numSV"},
             {"", "deg", "deg", "m", "m/s", "m/s", "m/s", "m", "m", "m", ""});

    int hMSL = 4000;
    for (int i = 0; i <= 295; ++i) {
        if (i >= 10)
            hMSL -= velDAt(i);

        const QByteArray time = QByteArray("2024-01-01T12:")
            + QByteArray::number(i / 60).rightJustified(2, '0') + ':'
            + QByteArray::number(i % 60).rightJustified(2, '0') + ".000Z";
        const QByteArray lat = QByteArray("45.") + QByteArray::number(i).rightJustified(4, '0');

        b.row("GNSS", time + ',' + lat + ",-75,"
                      + QByteArray::number(hMSL) + ','
                      + QByteArray::number(velNAt(i)) + ",0,"
                      + QByteArray::number(velDAt(i)) + ",1.0,1.0,0.5,12");
    }
    return b;
}

Fs2FileBuilder sensorFile(const QByteArray &sessionId)
{
    Fs2FileBuilder b = withStandardVars(sessionId);
    b.sensor("TIME", {"time", "tow", "week"}, {"s", "s", ""})
     .sensor("IMU",
             {"time", "wx", "wy", "wz", "ax", "ay", "az", "temperature"},
             {"s", "deg/s", "deg/s", "deg/s", "g", "g", "g", "deg C"})
     .sensor("MAG", {"time", "x", "y", "z", "temperature"},
             {"s", "gauss", "gauss", "gauss", "deg C"})
     .sensor("BARO", {"time", "pressure", "temperature"}, {"s", "Pa", "deg C"})
     .sensor("HUM", {"time", "humidity", "temperature"}, {"s", "percent", "deg C"})
     .sensor("VBAT", {"time", "voltage"}, {"s", "V"});

    b.row("TIME", "10,129610,2295").row("TIME", "20,129620,2295").row("TIME", "30,129630,2295");
    b.row("IMU", "10,3,4,0,0,0,1,40").row("IMU", "20,6,8,0,0,0,1,40").row("IMU", "30,9,12,0,0,0,1,40");
    for (const char *t : {"10", "20", "30"}) {
        b.row("MAG", QByteArray(t) + ",3,4,0,40");
        b.row("BARO", QByteArray(t) + ",90000,20");
        b.row("HUM", QByteArray(t) + ",50,20");
        b.row("VBAT", QByteArray(t) + ",4.0");
    }
    return b;
}

SessionData load(const QByteArray &sessionId)
{
    SessionData track = importOne(trackFile(sessionId), QStringLiteral("descent-track"));
    const SessionData sensor = importOne(sensorFile(sessionId), QStringLiteral("descent-sensor"));

    for (const QString &key : sensor.attributeKeys()) {
        if (!track.hasAttribute(key))
            track.setAttribute(key, sensor.storedAttribute(key));
    }
    // Source layer only: the merged session carries the data as recorded.
    track.mergeSourceData(sensor.sourceData());
    return track;
}

SessionData loadSensorOnly(const QByteArray &sessionId)
{
    return importOne(sensorFile(sessionId), QStringLiteral("descent-sensor-only"));
}

} // namespace DescentFixture

namespace {

constexpr double T0 = DescentFixture::T0;

GoldenValue attr(const char *key, const QVariant &value, double tolerance = 1e-6)
{
    return {DependencyKey::attribute(QString::fromLatin1(key)), true, value, {}, 0, tolerance};
}

GoldenValue noAttr(const char *key)
{
    return {DependencyKey::attribute(QString::fromLatin1(key)), false, QVariant(), {}, 0, 0.0};
}

GoldenValue meas(const char *sensor, const char *name, int count, const QList<GoldenSample> &samples,
                 double tolerance = 1e-9)
{
    return {DependencyKey::measurement(QString::fromLatin1(sensor), QString::fromLatin1(name)),
            true, QVariant(), samples, count, tolerance};
}

} // namespace

QList<GoldenValue> goldenValues()
{
    QList<GoldenValue> g;

    // ---- attributecalculations
    g << attr("_ANALYSIS_START_TIME", T0)
      << attr("_ANALYSIS_END_TIME", T0 + 295.0)
      << attr("_GROUND_ELEV", 100.0)
      << attr("_EXIT_TIME", T0 + 9.0)       // crossing at row 11, a = 1, accD = 5: t11 - 10/5
      << attr("_SYNC_TIME", T0 + 9.0)
      << attr("_COURSE_REF", T0 + 9.0)
      << attr("_LANDING_TIME", T0 + 266.0)
      << attr("_MANOEUVRE_START_TIME", T0 + 9.0)
      << attr("_FLARE_START_TIME", T0 + 199.0)
      << attr("_FLARE_END_TIME", T0 + 204.0)
      << attr("_MAX_VELD_TIME", T0 + 19.0)
      << attr("_MAX_VELH_TIME", T0 + 9.0)
      << attr("_START_TIME", T0)            // GNSS candidate
      << attr("_DURATION", 295.0);

    // ---- timecalculations (the fit outputs are strings)
    g << attr("_TIME_FIT_A", QStringLiteral("1"))
      << attr("_TIME_FIT_B", QStringLiteral("1704110400"));

    // ---- wspcalculations
    g << attr("_WSP_VERSION", QStringLiteral("1.0"))
      << attr("_WSP_TOP_ALT", 2500.0)
      << attr("_WSP_BOTTOM_ALT", 1500.0)
      << attr("_WSP_TASK", QStringLiteral("Time"))
      << attr("_WSP_REF1_TIME", T0 + 20.0)
      << attr("_WSP_ENTRY_TIME", T0 + 41.5)
      << attr("_WSP_EXIT_TIME", T0 + 61.5)
      << attr("_WSP_TIME_RESULT", 20.0)
      << attr("_WSP_ENTRY_LAT", 45.00415, 1e-9)
      << attr("_WSP_EXIT_LAT", 45.00615, 1e-9)
      << attr("_WSP_ENTRY_LON", -75.0, 1e-9)
      << attr("_WSP_EXIT_LON", -75.0, 1e-9)
      << attr("_WSP_DIST_RESULT", 222.26375611503698)        // captured at v2026.04.1 (approximately 222.3 m)
      << attr("_WSP_SPEED_RESULT", 11.113187805751849)      // captured at v2026.04.1 (approximately 11.1 m/s)
      << attr("_WSP_SEP_RESULT", 1.5381, 1e-4);

    // ---- spcalculations
    g << attr("_SP_PERF_WINDOW_HEIGHT", 2255.5199278233622, 1e-9)   // 7400 ft
      << attr("_SP_VAL_WINDOW_HEIGHT", 1005.839967813121, 1e-9)     // 3300 ft
      << attr("_SP_BREAKOFF_ALT", 1706.8799453798417, 1e-9)         // 5600 ft
      << attr("_SP_WINDOW_START_TIME", T0 + 11.0)
      << attr("_SP_WINDOW_START_ALT", 3885.0)
      << attr("_SP_WINDOW_END_TIME", 1704110457.362401, 1e-5)    // captured at v2026.04.1 (T0 + 57.3624)
      << attr("_SP_BEST_START_TIME", T0 + 18.0)
      << attr("_SP_BEST_END_TIME", T0 + 21.0)
      << attr("_SP_SPEED_RESULT", 50.0)
      << attr("_SP_MAX_SPEED_ACC", 0.47140452079103168, 1e-12);

    // ---- synthesized interpolation
    g << attr("_EXIT_TIME:GNSS/_time/hMSL", 4000.0)
      << attr("_WSP_ENTRY_TIME:GNSS/_time/hMSL", 2600.0)
      << noAttr("_NOPE:GNSS/_time/hMSL")
      << noAttr("_EXIT_TIME:GNSS/_time/nope")
      << noAttr("_EXIT_TIME:GNSS/hMSL");

    // ---- GNSS time
    g << meas("GNSS", "_time", 296, {{0, T0}, {295, T0 + 295.0}})
      << meas("GNSS", "_system_time", 296, {{0, 0.0}, {295, 295.0}});

    // ---- gnsscalculations
    g << meas("GNSS", "z", 296, {{0, 3900.0}, {295, 0.0}})
      << meas("GNSS", "velH", 296, {{0, 50.0}})
      << meas("GNSS", "vel", 296, {{19, 70.710678118654755}})
      << meas("GNSS", "accD", 296, {{0, 0.0}, {11, 5.0}})
      << meas("GNSS", "accN", 296, {{19, 0.0}})
      << meas("GNSS", "accE", 296, {{19, 0.0}})
      << meas("GNSS", "wcVel", 296, {{19, 70.710678118654755}})
      << meas("GNSS", "course", 296, {{0, 0.0}, {19, 0.0}, {295, 0.0}})
      << meas("GNSS", "courseRate", 296, {{19, 0.0}})
      << meas("GNSS", "glideRatio", 296, {{19, 1.0}})
      << meas("GNSS", "diveAngle", 296, {{19, 45.0}})
      << meas("GNSS", "diveAngleRate", 296, {{19, 1.5063937520916681}})          // captured
      << meas("GNSS", "accH", 296, {{19, 0.0}})
      << meas("GNSS", "wcVelH", 296, {{0, 50.0}})
      << meas("GNSS", "accAlongTrack", 296, {{19, 1.7677669529663687}})          // captured
      << meas("GNSS", "accCrossTrack", 296, {{19, 1.7677669529663689}})          // captured
      << meas("GNSS", "lift", 296, {{19, 0.0024510659095399903}}, 1e-15)        // captured
      << meas("GNSS", "drag", 296, {{19, 0.0024510659095399899}}, 1e-15)        // captured
      << meas("GNSS", "specificEnergy", 296, {{19, 38049.106249999997}})         // 0.5 * 5000 + 9.80665 * 3625
      << meas("GNSS", "specificEnergyRate", 296, {{19, -371.58250000000044}});   // captured

    // ---- sensor time
    for (const char *sensor : {"IMU", "BARO", "HUM", "MAG", "TIME", "VBAT"}) {
        g << meas(sensor, "_time", 3, {{0, T0 + 10.0}, {1, T0 + 20.0}, {2, T0 + 30.0}});
    }
    g << meas("IMU", "_system_time", 3, {{0, 10.0}, {1, 20.0}, {2, 30.0}});

    // ---- imucalculations / magcalculations
    // Recorded gyro (3, 4, 0), (6, 8, 0), (9, 12, 0) deg/s without SCHEMA_VER:
    // the legacy correction makes the magnitudes 5, 10, 15 x 1.14688. aTotal
    // and MAG/total come through the conversion from recorded 1 g / gauss.
    g << meas("IMU", "wTotal", 3, {{0, 5.7344}, {1, 11.4688}, {2, 17.2032}})
      << meas("IMU", "aTotal", 3, {{0, 9.80665}, {1, 9.80665}, {2, 9.80665}})
      << meas("MAG", "total", 3, {{0, 0.0005}, {1, 0.0005}, {2, 0.0005}}, 1e-15);

    // ---- localcoordinatecalculations
    // Every row has hAcc = 1.0, so the origin is row 0 (45 N, 75 W, 4000 m).
    // The whole track lies on the origin's meridian, and row 0 flies north at
    // 50 m/s. Derived by hand.
    g << attr("_LOCAL_ORIGIN_LAT", 45.0, 0.0)
      << attr("_LOCAL_ORIGIN_LON", -75.0, 0.0)
      << attr("_LOCAL_ORIGIN_HMSL", 4000.0, 0.0)
      << attr("_LOCAL_ORIGIN_INDEX", 0.0, 0.0)
      << meas("Local", "north", 296, {{0, 0.0}}, 1e-6)
      << meas("Local", "east", 296, {{0, 0.0}, {295, 0.0}}, 1e-6)   // same meridian as the origin
      << meas("Local", "down", 296, {{0, 0.0}}, 1e-6)
      << meas("Local", "velN", 296, {{0, 50.0}})
      << meas("Local", "velE", 296, {{0, 0.0}, {295, 0.0}})        // rotation about the east axis only
      << meas("Local", "velD", 296, {{0, 0.0}})
      << meas("Local", "_time", 296, {{0, T0}, {295, T0 + 295.0}})
      << meas("Local", "_system_time", 296, {{0, 0.0}, {295, 295.0}});

    // ---- simplificationcalculations
    g << meas("Simplified", "lat", 2, {{0, 45.0}, {1, 45.0295}})
      << meas("Simplified", "lon", 2, {{0, -75.0}, {1, -75.0}})
      << meas("Simplified", "hMSL", 2, {{0, 4000.0}, {1, 100.0}})
      << meas("Simplified", "_time", 2, {{0, T0}, {1, T0 + 295.0}})
      << meas("Simplified", "north", 2, {{0, 0.0}}, 1e-6)
      << meas("Simplified", "east", 2, {{0, 0.0}, {1, 0.0}}, 1e-6)   // same meridian as the origin
      << meas("Simplified", "down", 2, {{0, 0.0}}, 1e-6);

    return g;
}

QString goldenTag(const DependencyKey &name)
{
    return describe(name);
}

QString compareToGolden(const GoldenValue &golden, const QVariant &attribute,
                        const QVector<double> &samples)
{
    const QString tag = goldenTag(golden.name);

    if (golden.name.type == DependencyKey::Type::Attribute) {
        if (!golden.available) {
            return attribute.isValid()
                ? QStringLiteral("%1: expected unavailable, got %2").arg(tag, attribute.toString())
                : QString();
        }
        if (!attribute.isValid())
            return QStringLiteral("%1: unavailable").arg(tag);

        if (golden.attribute.userType() == QMetaType::QString) {
            if (attribute.userType() != QMetaType::QString)
                return QStringLiteral("%1: expected a QString, got type %2")
                    .arg(tag, QString::fromLatin1(attribute.typeName()));
            if (attribute.toString() != golden.attribute.toString())
                return QStringLiteral("%1: expected \"%2\", got \"%3\"")
                    .arg(tag, golden.attribute.toString(), attribute.toString());
            return QString();
        }

        bool ok = false;
        const double actual = attribute.toDouble(&ok);
        const double expected = golden.attribute.toDouble();
        if (!ok || !(std::abs(actual - expected) <= golden.tolerance))
            return QStringLiteral("%1: expected %2, got %3")
                .arg(tag, QString::number(expected, 'g', 17), QString::number(actual, 'g', 17));
        return QString();
    }

    if (!golden.available) {
        return samples.isEmpty() ? QString()
                                 : QStringLiteral("%1: expected unavailable, got %2 samples")
                                       .arg(tag).arg(samples.size());
    }
    if (samples.size() != golden.sampleCount)
        return QStringLiteral("%1: expected %2 samples, got %3")
            .arg(tag).arg(golden.sampleCount).arg(samples.size());
    for (const GoldenSample &s : golden.samples) {
        const double actual = samples.at(s.index);
        if (!(std::abs(actual - s.value) <= golden.tolerance))
            return QStringLiteral("%1[%2]: expected %3, got %4")
                .arg(tag).arg(s.index)
                .arg(QString::number(s.value, 'g', 17), QString::number(actual, 'g', 17));
    }
    return QString();
}

void copyStoredState(const SessionData &from, FakeSessionState &to)
{
    // Stored state only, never effective values: the copy must carry the data
    // as recorded (e.g. 1 "g"), not what the conversion layer makes of it.
    for (const QString &key : from.attributeKeys())
        to.setAttribute(key, from.storedAttribute(key));

    const SourceData source = from.sourceData();
    for (auto sensorIt = source.constBegin(); sensorIt != source.constEnd(); ++sensorIt) {
        for (auto it = sensorIt.value().constBegin(); it != sensorIt.value().constEnd(); ++it)
            to.setMeasurement(sensorIt.key(), it.key(), it.value().samples, it.value().unit);
    }
}

} // namespace FlySightTest
