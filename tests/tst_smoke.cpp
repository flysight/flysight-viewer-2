// Characterization ("smoke") suite.
//
// Drives importer, session, calculations, exporter, logbook and model end to
// end. It started as a pin of v2026.04.1; expectations that a phase changed on
// purpose were rewritten with that phase. Every expectation is a literal.
// Corrected gyro values (recorded x 1.14688) are compared with an absolute
// tolerance of 1e-9: 62.5 * 1.14688 is not the double nearest 71.68.

#include <cstring>

#include <QDir>
#include <QRegularExpression>
#include <QSet>
#include <QtTest>

#include "dataexporter.h"
#include "dataimporter.h"
#include "fixturebuilder.h"
#include "logbookmanager.h"
#include "sessiondata.h"
#include "sessionmodel.h"
#include "testenvironment.h"
#include "testmain.h"

using namespace FlySight;
using namespace FlySightTest;

namespace {

bool isUnderRoot(const QString &path)
{
    const QString root = TestEnvironment::instance().rootPath() + QLatin1Char('/');
    return QDir::cleanPath(path).startsWith(root, Qt::CaseInsensitive);
}

QStringList sessionCsvFiles()
{
    return QDir(TestEnvironment::instance().sessionsDir())
        .entryList({QStringLiteral("*.csv")}, QDir::Files, QDir::Name);
}

} // namespace

class SmokeTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void importFs2Sensor();
    void importAppliesCreationDefaults();
    void importFs2Track();
    void importFs1();
    void rejectsUnknownFormat();
    void derivedMeasurement();
    void exportReloadRoundTrip();
    void logbookSaveReload();
    void modelMergeSavesToTempLogbook();
    void modelMergesTrackAndSensor();

private:
    // Writes the canned fixture into a fresh temp dir and imports it.
    bool importSensor(SessionData &session, QString *inputPath = nullptr);
    bool importTrack(SessionData &session);
};

void SmokeTest::initTestCase()
{
    TestEnvironment::instance().registerBuiltIns();
}

void SmokeTest::init()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();
    env.resetPreferencesToDefaults();
}

bool SmokeTest::importSensor(SessionData &session, QString *inputPath)
{
    const QString path = TestEnvironment::instance().newTempDir(QStringLiteral("sensor"))
                         + QStringLiteral("/sensor.csv");
    if (inputPath)
        *inputPath = path;
    if (!Fixtures::sensorFile().write(path))
        return false;
    DataImporter importer;
    return importer.importFile(path, session);
}

bool SmokeTest::importTrack(SessionData &session)
{
    const QString path = TestEnvironment::instance().newTempDir(QStringLiteral("track"))
                         + QStringLiteral("/track.csv");
    if (!Fixtures::trackFile().write(path))
        return false;
    DataImporter importer;
    return importer.importFile(path, session);
}

void SmokeTest::importFs2Sensor()
{
    SessionData session;
    QString input;
    QVERIFY(importSensor(session, &input));
    const QByteArray original = Fixtures::sensorFile().toBytes();

    QCOMPARE(session.sensorKeys(), QStringList({"IMU", "MAG"}));

    QCOMPARE(session.getMeasurement("IMU", "time"), QVector<double>({3.0}));

    // The file declares no SCHEMA_VER, so its gyro rates are legacy-scaled:
    // ordinary (effective) reads are corrected by 1.14688, the source layer
    // keeps what was recorded.
    const QVector<double> wx = session.getMeasurement("IMU", "wx");
    const QVector<double> wy = session.getMeasurement("IMU", "wy");
    QCOMPARE(wx.size(), 1);
    QCOMPARE(wy.size(), 1);
    QVERIFY(qAbs(wx.at(0) - 71.68) <= 1e-9);
    QVERIFY(qAbs(wy.at(0) - -143.36) <= 1e-9);
    QCOMPARE(session.getMeasurement("IMU", "wz"), QVector<double>({0.0}));
    QCOMPARE(session.effectiveUnit("IMU", "wx"), QStringLiteral("deg/s"));
    QCOMPARE(session.sourceMeasurement("IMU", "wx"), QVector<double>({62.5}));
    QCOMPARE(session.sourceMeasurement("IMU", "wy"), QVector<double>({-125.0}));
    QCOMPARE(session.sourceUnit("IMU", "wx"), QStringLiteral("deg/s"));

    // g -> m/s^2 happens in the conversion layer; nothing is converted at import.
    QCOMPARE(session.getMeasurement("IMU", "ax"), QVector<double>({9.80665}));
    QCOMPARE(session.effectiveUnit("IMU", "ax"), QStringLiteral("m/s^2"));
    QCOMPARE(session.sourceMeasurement("IMU", "ax"), QVector<double>({1.0}));
    QCOMPARE(session.sourceUnit("IMU", "ax"), QStringLiteral("g"));

    // Likewise gauss -> T and the "deg C" -> "degC" label.
    QCOMPARE(session.getMeasurement("MAG", "x"), QVector<double>({0.0001}));
    QCOMPARE(session.effectiveUnit("MAG", "x"), QStringLiteral("T"));
    QCOMPARE(session.sourceMeasurement("MAG", "x"), QVector<double>({1.0}));
    QCOMPARE(session.sourceUnit("MAG", "x"), QStringLiteral("gauss"));
    QCOMPARE(session.getMeasurement("IMU", "temperature"), QVector<double>({40.0}));
    QCOMPARE(session.effectiveUnit("IMU", "temperature"), QStringLiteral("degC"));
    QCOMPARE(session.sourceUnit("IMU", "temperature"), QStringLiteral("deg C"));

    // Header attributes are kept as recorded and nothing is stamped.
    QVERIFY(!session.hasAttribute("SCHEMA_VER"));
    QCOMPARE(session.getAttribute("FIRMWARE_VER").toString(), QStringLiteral("v2023.09.22"));
    QCOMPARE(session.getAttribute("SESSION_ID").toString(), QStringLiteral("test-session"));
    QCOMPARE(session.getAttribute("DEVICE_ID").toString(), QStringLiteral("test-device"));

    // Importing never modifies the input file.
    QCOMPARE(readFileBytes(input), original);
}

void SmokeTest::importAppliesCreationDefaults()
{
    SessionData session;
    QVERIFY(importSensor(session));

    QCOMPARE(session.getAttribute("_DESCRIPTION").toString(), QStringLiteral("sensor.csv"));

    QVERIFY(session.hasAttribute("_IMPORT_TIME"));
    QVERIFY(session.getAttribute("_IMPORT_TIME").toDouble() > 0.0);

    QVERIFY(session.hasAttribute("_WIND_N"));
    QVERIFY(session.hasAttribute("_WIND_E"));
    QCOMPARE(session.getAttribute("_WIND_N").toDouble(), 0.0);
    QCOMPARE(session.getAttribute("_WIND_E").toDouble(), 0.0);

    QVERIFY(session.hasAttribute("_JUMPER_MASS"));
    QVERIFY(session.hasAttribute("_PLANFORM_AREA"));
    QCOMPARE(session.getAttribute("_JUMPER_MASS").toDouble(), 1.0);
    QCOMPARE(session.getAttribute("_PLANFORM_AREA").toDouble(), 1.0);

    // "Automatic" ground reference: nothing is baked in at import.
    QVERIFY(!session.hasAttribute("_GROUND_ELEV"));
}

void SmokeTest::importFs2Track()
{
    SessionData session;
    QVERIFY(importTrack(session));

    QCOMPARE(session.sensorKeys(), QStringList({"GNSS"}));
    QCOMPARE(session.getMeasurement("GNSS", "hMSL"), QVector<double>({4000.0, 3999.0, 3998.0}));
    QCOMPARE(session.getMeasurement("GNSS", "velN"), QVector<double>({10.0, 10.5, 11.0}));
    QCOMPARE(session.getMeasurement("GNSS", "numSV"), QVector<double>({12.0, 12.0, 13.0}));
    QCOMPARE(session.sourceUnit("GNSS", "hMSL"), QStringLiteral("m"));

    // ISO "Z" timestamps become seconds since the epoch at millisecond precision.
    const QVector<double> time = session.getMeasurement("GNSS", "time");
    QCOMPARE(time.size(), 3);
    QVERIFY(qAbs(time.at(0) - 1704110400.0) < 1e-6);
    QVERIFY(qAbs(time.at(1) - 1704110400.2) < 1e-6);
    QVERIFY(qAbs(time.at(2) - 1704110400.4) < 1e-6);
}

void SmokeTest::importFs1()
{
    const QString path = TestEnvironment::instance().newTempDir(QStringLiteral("fs1"))
                         + QStringLiteral("/fs1.csv");
    Fs1FileBuilder builder;
    builder.columns({"time", "lat", "lon", "hMSL", "velN", "velE", "velD",
                     "hAcc", "vAcc", "sAcc", "heading", "cAcc", "gpsFix", "numSV"})
           .units({"", "(deg)", "(deg)", "(m)", "(m/s)", "(m/s)", "(m/s)",
                   "(m)", "(m)", "(m/s)", "(deg)", "(deg)", "", ""})
           .row("2024-01-01T12:00:00.00Z,45.5,-73.25,4000.5,10,-20,5,1.5,2.5,0.25,296.5,1.25,3,12")
           .row("2024-01-01T12:00:00.20Z,45.5001,-73.2501,3999.5,10.5,-20.5,5.5,1.5,2.5,0.25,297.5,1.25,3,13");
    QVERIFY(builder.write(path));

    DataImporter importer;
    SessionData session;
    QVERIFY2(importer.importFile(path, session), qPrintable(importer.getLastError()));

    QCOMPARE(session.sensorKeys(), QStringList({"GNSS"}));
    QCOMPARE(session.getMeasurement("GNSS", "lat"), QVector<double>({45.5, 45.5001}));
    QCOMPARE(session.getMeasurement("GNSS", "lon"), QVector<double>({-73.25, -73.2501}));
    QCOMPARE(session.getMeasurement("GNSS", "hMSL"), QVector<double>({4000.5, 3999.5}));
    QCOMPARE(session.getMeasurement("GNSS", "velD"), QVector<double>({5.0, 5.5}));
    QCOMPARE(session.getMeasurement("GNSS", "heading"), QVector<double>({296.5, 297.5}));
    QCOMPARE(session.getMeasurement("GNSS", "numSV"), QVector<double>({12.0, 13.0}));

    const QVector<double> time = session.getMeasurement("GNSS", "time");
    QCOMPARE(time.size(), 2);
    QVERIFY(qAbs(time.at(0) - 1704110400.0) < 1e-6);
    QVERIFY(qAbs(time.at(1) - 1704110400.2) < 1e-6);

    // The parenthesized FS1 unit text is kept as recorded; the conversion
    // layer normalizes the label.
    QCOMPARE(session.sourceUnit("GNSS", "velN"), QStringLiteral("(m/s)"));
    QCOMPARE(session.sourceUnit("GNSS", "hMSL"), QStringLiteral("(m)"));
    QCOMPARE(session.effectiveUnit("GNSS", "velN"), QStringLiteral("m/s"));
    QCOMPARE(session.effectiveUnit("GNSS", "hMSL"), QStringLiteral("m"));

    // FS1 files carry no SESSION_ID; one is synthesized from the MD5 of the file.
    const QString sessionId = session.getAttribute("SESSION_ID").toString();
    static const QRegularExpression md5Hex(QStringLiteral("^[0-9a-f]{32}$"));
    QVERIFY2(md5Hex.match(sessionId).hasMatch(), qPrintable(sessionId));
}

void SmokeTest::rejectsUnknownFormat()
{
    const QString path = TestEnvironment::instance().newTempDir(QStringLiteral("bad"))
                         + QStringLiteral("/bad.csv");
    QVERIFY(writeFile(path, "hello\nworld\n"));

    DataImporter importer;
    SessionData session;
    QVERIFY(!importer.importFile(path, session));
    QCOMPARE(importer.getLastError(), QStringLiteral("Unknown file format"));
    QVERIFY(session.sensorKeys().isEmpty());
    QVERIFY(session.attributeKeys().isEmpty());
}

void SmokeTest::derivedMeasurement()
{
    SessionData session;
    QVERIFY(importSensor(session));

    // Derived IMU/wTotal is computed from the corrected gyro rates:
    // sqrt(71.68^2 + 143.36^2 + 0^2) = 139.75424859373686 x 1.14688.
    const QVector<double> wTotal = session.getMeasurement("IMU", "wTotal");
    QCOMPARE(wTotal.size(), 1);
    QVERIFY(qAbs(wTotal.at(0) - 160.28135262718) <= 1e-9);

    // A derived value never shows up as stored data, and has no source.
    QVERIFY(!session.hasMeasurement("IMU", "wTotal"));
    QVERIFY(!session.measurementKeys("IMU").contains(QStringLiteral("wTotal")));
    QVERIFY(session.sourceMeasurement("IMU", "wTotal").isEmpty());
}

void SmokeTest::exportReloadRoundTrip()
{
    SessionData session;
    QVERIFY(importSensor(session));

    // Warm the calculated-value cache: derived values must never be written.
    QCOMPARE(session.getMeasurement("IMU", "wTotal").size(), 1);

    const QString dir = TestEnvironment::instance().newTempDir(QStringLiteral("export"));
    const QString firstPath = dir + QStringLiteral("/first.csv");
    QVERIFY(DataExporter::exportSession(firstPath, session));

    const QByteArray firstBytes = readFileBytes(firstPath);
    QVERIFY(firstBytes.startsWith("$FLYS,1\n"));
    QVERIFY(!firstBytes.contains("SCHEMA_VER"));
    QVERIFY(!firstBytes.contains("wTotal"));

    DataImporter importer;
    SessionData reloaded;
    QVERIFY2(importer.readFile(firstPath, reloaded), qPrintable(importer.getLastError()));

    QCOMPARE(reloaded.sensorKeys(), QStringList({"IMU", "MAG"}));
    QCOMPARE(reloaded.measurementKeys("IMU"),
             QStringList({"ax", "temperature", "time", "wx", "wy", "wz"}));
    QCOMPARE(reloaded.measurementKeys("MAG"),
             QStringList({"temperature", "time", "x", "y", "z"}));

    // A saved file holds the source layer: the recorded values under the
    // recorded unit text. Effective values are the same before and after.
    struct Expected { const char *sensor; const char *name; double source; const char *sourceUnit;
                      double effective; const char *effectiveUnit; };
    const Expected expected[] = {
        {"IMU", "time",        3.0,    "s",     3.0,      "s"},
        {"IMU", "wx",          62.5,   "deg/s", 71.68,    "deg/s"},
        {"IMU", "wy",          -125.0, "deg/s", -143.36,  "deg/s"},
        {"IMU", "wz",          0.0,    "deg/s", 0.0,      "deg/s"},
        {"IMU", "ax",          1.0,    "g",     9.80665,  "m/s^2"},
        {"IMU", "temperature", 40.0,   "deg C", 40.0,     "degC"},
        {"MAG", "time",        3.0,    "s",     3.0,      "s"},
        {"MAG", "x",           1.0,    "gauss", 0.0001,   "T"},
        {"MAG", "y",           0.0,    "gauss", 0.0,      "T"},
        {"MAG", "z",           -0.5,   "gauss", -0.00005, "T"},
        {"MAG", "temperature", 40.0,   "deg C", 40.0,     "degC"},
    };
    for (const Expected &e : expected) {
        const QVector<double> source = reloaded.sourceMeasurement(e.sensor, e.name);
        QVERIFY2(source.size() == 1, e.name);
        // Bit equality: a saved sample reloads exactly (spec 9.2).
        const double reloadedSource = source.at(0);
        QVERIFY2(std::memcmp(&reloadedSource, &e.source, sizeof(double)) == 0, e.name);
        QCOMPARE(reloaded.sourceUnit(e.sensor, e.name), QString::fromLatin1(e.sourceUnit));

        const QVector<double> effective = reloaded.getMeasurement(e.sensor, e.name);
        QVERIFY2(effective.size() == 1, e.name);
        QVERIFY2(qAbs(effective.at(0) - e.effective) <= 1e-9, e.name);
        QCOMPARE(reloaded.effectiveUnit(e.sensor, e.name), QString::fromLatin1(e.effectiveUnit));
        QCOMPARE(effective, session.getMeasurement(e.sensor, e.name));
        QCOMPARE(reloaded.effectiveUnit(e.sensor, e.name), session.effectiveUnit(e.sensor, e.name));
    }

    QCOMPARE(reloaded.getAttribute("FIRMWARE_VER").toString(), QStringLiteral("v2023.09.22"));
    QCOMPARE(reloaded.getAttribute("SESSION_ID").toString(), QStringLiteral("test-session"));
    QCOMPARE(reloaded.getAttribute("DEVICE_ID").toString(), QStringLiteral("test-device"));
    QCOMPARE(reloaded.getAttribute("_DESCRIPTION").toString(), QStringLiteral("sensor.csv"));
    QVERIFY(!reloaded.hasAttribute("SCHEMA_VER"));

    // Saving what was loaded changes nothing.
    const QString secondPath = dir + QStringLiteral("/second.csv");
    QVERIFY(DataExporter::exportSession(secondPath, reloaded));
    QCOMPARE(readFileBytes(secondPath), firstBytes);
}

void SmokeTest::logbookSaveReload()
{
    TestEnvironment &env = TestEnvironment::instance();
    LogbookManager &logbook = LogbookManager::instance();

    SessionData session;
    QVERIFY(importSensor(session));

    logbook.initialize();
    QVERIFY(logbook.saveSession(session));
    logbook.flushIndex();

    QVERIFY(isUnderRoot(env.sessionsDir()));
    QCOMPARE(sessionCsvFiles().size(), 1);
    QVERIFY(QFileInfo::exists(env.indexPath()));
    QVERIFY(isUnderRoot(env.indexPath()));

    // Simulated restart: the index on disk is all that survives.
    env.reopenLogbook();
    QVERIFY(!logbook.hasIndexData());
    logbook.initialize();
    QVERIFY(logbook.hasIndexData());
    QVERIFY(!logbook.hasDeferredScan());

    const std::optional<SessionData> loaded = logbook.loadSession(QStringLiteral("test-session"));
    QVERIFY(loaded.has_value());

    // The saved file holds the recorded rate; reads return the corrected one.
    const QVector<double> wx = loaded->getMeasurement("IMU", "wx");
    QCOMPARE(wx.size(), 1);
    QVERIFY(qAbs(wx.at(0) - 71.68) <= 1e-9);
    QCOMPARE(loaded->sourceMeasurement("IMU", "wx"), QVector<double>({62.5}));
    QCOMPARE(loaded->getAttribute("FIRMWARE_VER").toString(), QStringLiteral("v2023.09.22"));
    QVERIFY(!loaded->hasAttribute("SCHEMA_VER"));

    QVERIFY(!logbook.loadSession(QStringLiteral("no-such-session")).has_value());
}

void SmokeTest::modelMergeSavesToTempLogbook()
{
    TestEnvironment &env = TestEnvironment::instance();
    LogbookManager::instance().initialize();

    SessionData track;
    QVERIFY(importTrack(track));

    SessionModel model;
    model.mergeSessions({track});

    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.getSessionRow(QStringLiteral("test-session")), 0);

    // The save is deferred to the idle scheduler.
    QVERIFY(sessionCsvFiles().isEmpty());
    QVERIFY(waitForIdle(model));

    QCOMPARE(sessionCsvFiles().size(), 1);
    QVERIFY(QFileInfo::exists(env.indexPath()));
    QVERIFY(isUnderRoot(env.sessionsDir()));
    QVERIFY(!model.rowAt(0).dirty);

    // Nothing left to do: returns immediately.
    QVERIFY(waitForIdle(model));
}

void SmokeTest::modelMergesTrackAndSensor()
{
    LogbookManager::instance().initialize();

    SessionData track;
    SessionData sensor;
    QVERIFY(importTrack(track));
    QVERIFY(importSensor(sensor));

    SessionModel model;
    const QList<MergeResult> first = model.mergeSessions({track});
    const QList<MergeResult> second = model.mergeSessions({sensor});

    // TRACK and SENSOR files with one SESSION_ID end up as one session holding
    // both files' sensors: the first creates it, the second merges into it.
    // (The merge rules themselves are covered by tst_session_merge and
    // tst_import_merge.)
    QCOMPARE(first.size(), 1);
    QVERIFY(first.first().outcome == MergeResult::Outcome::Created);
    QCOMPARE(second.size(), 1);
    QVERIFY(second.first().outcome == MergeResult::Outcome::Merged);
    QVERIFY(second.first().error.isEmpty());
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.sessionRef(0).sensorKeys(), QStringList({"GNSS", "IMU", "MAG"}));

    // Leave no deferred save behind for the next test.
    model.flushDirtySessions();
    QCOMPARE(sessionCsvFiles().size(), 1);
}

FLYSIGHT_TEST_MAIN(SmokeTest)

#include "tst_smoke.moc"
