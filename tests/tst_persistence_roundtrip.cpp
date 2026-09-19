// Save / reload round trip on the real importer, exporter and logbook
// (spec 9.1 - 9.3):
//
//   acceptance 5 - source samples bit-identical, source units and header
//                  attributes preserved, SCHEMA_VER absent if it was absent,
//                  effective values unchanged, a second cycle changes nothing,
//                  and none of it depends on the state of any cache;
//   acceptance 6 - a logbook written by a released version loads, and a save
//                  does not rescale, relabel, or stamp it.
//
// Every expected number, line and file is a literal. The only computed
// comparisons are "A equals B" (which acceptance 5 asks for) and the engine's
// fresh-evaluation oracle.
//
// Exporter-level tests reload with DataImporter::readFile, never with
// LogbookManager::loadSession, so the legacy mass / area / wind backfill of
// loadSession cannot interfere; logbook-level tests use fixtures that already
// carry those four attributes, except the one test that pins the backfill.

#include <QtTest>

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <limits>

#include <QDateTime>
#include <QDir>
#include <QSet>
#include <QTimeZone>

#include "dataexporter.h"
#include "dataimporter.h"
#include "engine/calculationengine.h"
#include "fixturebuilder.h"
#include "logbookmanager.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "sessiondata.h"
#include "testenvironment.h"
#include "testmain.h"

using namespace FlySight;
using namespace FlySightTest;

namespace {

const double kInf = std::numeric_limits<double>::infinity();
const double kNaN = std::numeric_limits<double>::quiet_NaN();

bool sameBits(double a, double b)
{
    return std::memcmp(&a, &b, sizeof(double)) == 0;
}

bool bitsEqual(const QVector<double> &actual, std::initializer_list<double> expected)
{
    if (actual.size() != qsizetype(expected.size()))
        return false;
    qsizetype i = 0;
    for (double e : expected) {
        if (!sameBits(actual.at(i++), e))
            return false;
    }
    return true;
}

bool bitsEqual(const QVector<double> &a, const QVector<double> &b)
{
    if (a.size() != b.size())
        return false;
    for (qsizetype i = 0; i < a.size(); ++i) {
        if (!sameBits(a.at(i), b.at(i)))
            return false;
    }
    return true;
}

bool isNear(double a, double b)
{
    return qAbs(a - b) <= 1e-9;
}

QStringList g_warnings;

void collectingHandler(QtMsgType type, const QMessageLogContext &, const QString &message)
{
    if (type == QtWarningMsg || type == QtCriticalMsg)
        g_warnings.append(message);
}

// Collects warnings for as long as it lives. Installed after registerBuiltIns().
class WarningCollector {
public:
    WarningCollector()
    {
        g_warnings.clear();
        m_previous = qInstallMessageHandler(collectingHandler);
    }
    ~WarningCollector() { qInstallMessageHandler(m_previous); }
    int count(const QString &fragment) const
    {
        int n = 0;
        for (const QString &w : std::as_const(g_warnings))
            n += w.contains(fragment) ? 1 : 0;
        return n;
    }

private:
    QtMessageHandler m_previous = nullptr;
};

QString tempFile(const QString &stem)
{
    return TestEnvironment::instance().newTempDir(stem) + QLatin1Char('/') + stem + QStringLiteral(".csv");
}

// The file the exporter would write, or a null array (with a test failure
// message on stderr) when it refuses.
QByteArray exportBytes(const SessionData &session)
{
    QString error;
    const std::optional<QByteArray> bytes = DataExporter::toBytes(session, &error);
    if (!bytes) {
        qWarning("exportBytes: %s", qPrintable(error));
        return QByteArray();
    }
    return *bytes;
}

// Parses bytes without device initialization: exactly what the file says.
bool reload(const QByteArray &bytes, SessionData &session)
{
    const QString path = tempFile(QStringLiteral("reload"));
    if (!writeFile(path, bytes))
        return false;
    DataImporter importer;
    if (!importer.readFile(path, session)) {
        qWarning("reload: %s", qPrintable(importer.getLastError()));
        return false;
    }
    return true;
}

bool readBuilder(const Fs2FileBuilder &file, SessionData &session)
{
    return reload(file.toBytes(), session);
}

// Values chosen to be awkward for a text format: an ISO timestamp with
// milliseconds, negative zero, a denormal, the largest double, a value with
// 16 significant digits, commas and '=' in header values, an empty header
// value, an empty unit, a custom sensor with a '#' in a column label.
Fs2FileBuilder awkwardFile()
{
    Fs2FileBuilder b;
    b.var("FIRMWARE_VER", "v2023.09.22")
     .var("DEVICE_ID", "test-device")
     .var("SESSION_ID", "awkward")
     .var("CUSTOM_KEY", "a=b, with, commas")
     .var("EMPTY_VAL", "")
     .var("_DESCRIPTION", "Perris, run 2, windy")
     .sensor("GNSS", {"time", "lat", "hMSL"}, {"", "deg", "m"})
     .sensor("IMU", {"time", "wx", "ax"}, {"s", "deg/s", "g"})
     .sensor("FOO", {"time", "bar#1"}, {"s", "furlongs"})
     .row("GNSS", "2024-06-20T16:13:20.123Z,45.1234567,0.1")
     .row("GNSS", "2024-06-20T16:13:20.323Z,-0,1e-320")
     .row("IMU", "3,62.5,0.3333333333333333")
     .row("IMU", "4,-125,1.7976931348623157e+308")
     .row("FOO", "3,7");
    return b;
}

// A file as a released Viewer version wrote it (exporter order, physical
// units, no SCHEMA_VER), with the lossy six-digit _IMPORT_TIME those versions
// produced. withAeroAndWind = the four attributes loadSession() backfills.
// magX = the text of the one MAG/x sample (in tesla). The default, 0.25, has
// the same text in the released 15-significant-digit form and in the shortest
// round-trip form; "0.0001" does not (see releasedValueWithDifferentText).
QByteArray releasedFile(bool withAeroAndWind, const QByteArray &magX = "0.25")
{
    QByteArray r =
        "$FLYS,1\n"
        "$VAR,FIRMWARE_VER,v2023.09.22\n"
        "$VAR,DEVICE_ID,test-device\n"
        "$VAR,SESSION_ID,rel\n"
        "$VAR,_DESCRIPTION,old jump\n"
        "$VAR,_IMPORT_TIME,1.7189e+09\n";
    if (withAeroAndWind) {
        r += "$VAR,_JUMPER_MASS,80\n"
             "$VAR,_PLANFORM_AREA,2\n"
             "$VAR,_WIND_E,0\n"
             "$VAR,_WIND_N,0\n";
    }
    r += "$COL,MAG,time,x\n"
         "$UNIT,MAG,s,T\n"
         "$COL,IMU,time,wx,wy,wz,ax,temperature\n"
         "$UNIT,IMU,s,deg/s,deg/s,deg/s,m/s^2,degC\n"
         "$DATA\n"
         "$MAG,3," + magX + "\n"
         "$IMU,3,62.5,-125,0,9.80665,40\n";
    return r;
}

QStringList sessionCsvFiles()
{
    return QDir(TestEnvironment::instance().sessionsDir())
        .entryList({QStringLiteral("*.csv")}, QDir::Files, QDir::Name);
}

// Lets the logbook create the index entry for "rel", then replaces the file
// with `bytes`. Returns the path of the session file.
QString installReleasedFile(const QByteArray &bytes)
{
    LogbookManager &logbook = LogbookManager::instance();

    SessionData placeholder;
    placeholder.setAttribute("SESSION_ID", QStringLiteral("rel"));
    placeholder.setAttribute("DEVICE_ID", QStringLiteral("test-device"));
    placeholder.setSourceMeasurement("IMU", "time", {3.0}, "s");
    if (!logbook.saveSession(placeholder))
        return QString();

    const QStringList files = sessionCsvFiles();
    if (files.size() != 1)
        return QString();
    const QString path = TestEnvironment::instance().sessionsDir() + QLatin1Char('/') + files.first();
    return writeFile(path, bytes) ? path : QString();
}

QList<QByteArray> lines(const QByteArray &bytes)
{
    QList<QByteArray> result = bytes.split('\n');
    if (!result.isEmpty() && result.last().isEmpty())
        result.removeLast();
    return result;
}

} // namespace

class PersistenceRoundTripTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    // acceptance 5
    void samplesAreBitIdentical();
    void unitsAndHeaderAttributesPreserved();
    void schemaVerOnlyIfRecorded();
    void effectiveValuesUnchanged();
    void secondCycleIsByteIdentical();
    void canonicalInputIsReproduced();
    void typedAttributesRoundTrip();
    void warmAndColdCachesSameFile();

    // spec 9.2: non-finite values; what cannot be written
    void nonFiniteRoundTrip();
    void raggedSensorIsRejected();
    void headerOnlySensor();
    void unrepresentableText();

    // acceptance 6
    void releasedLogbookSaveKeepsBytes();
    void releasedValueWithDifferentText();
    void releasedLogbookBackfillIsAdditive();

    // acceptance 5 through the logbook
    void logbookSaveReloadCycle();
};

void PersistenceRoundTripTest::initTestCase()
{
    TestEnvironment::instance().registerBuiltIns();
}

void PersistenceRoundTripTest::init()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();
    env.resetPreferencesToDefaults();
}

// Acceptance 5: source samples are bit-identical after save and reload.
void PersistenceRoundTripTest::samplesAreBitIdentical()
{
    SessionData a;
    QVERIFY(readBuilder(awkwardFile(), a));

    const QByteArray bytes = exportBytes(a);
    QVERIFY(!bytes.isEmpty());
    SessionData b;
    QVERIFY(reload(bytes, b));

    // Against literals. The ISO timestamps come back as exact seconds.
    QVERIFY(bitsEqual(b.sourceMeasurement("GNSS", "time"), {1718900000.123, 1718900000.323}));
    QVERIFY(bitsEqual(b.sourceMeasurement("GNSS", "lat"), {45.1234567, -0.0}));
    QVERIFY(std::signbit(b.sourceMeasurement("GNSS", "lat").at(1)));
    QVERIFY(bitsEqual(b.sourceMeasurement("GNSS", "hMSL"), {0.1, 1e-320}));
    QVERIFY(bitsEqual(b.sourceMeasurement("IMU", "time"), {3.0, 4.0}));
    QVERIFY(bitsEqual(b.sourceMeasurement("IMU", "wx"), {62.5, -125.0}));
    QVERIFY(bitsEqual(b.sourceMeasurement("IMU", "ax"), {0.3333333333333333, 1.7976931348623157e308}));
    QVERIFY(bitsEqual(b.sourceMeasurement("FOO", "time"), {3.0}));
    QVERIFY(bitsEqual(b.sourceMeasurement("FOO", "bar#1"), {7.0}));

    // Against the session that was saved, column by column and bit by bit
    // (operator== alone would call -0.0 and 0.0 equal).
    const SourceData sourceA = a.sourceData();
    const SourceData sourceB = b.sourceData();
    QCOMPARE(sourceB.keys(), QStringList({"FOO", "GNSS", "IMU"}));
    for (auto sensorIt = sourceA.cbegin(); sensorIt != sourceA.cend(); ++sensorIt) {
        QCOMPARE(sourceB.value(sensorIt.key()).keys(), sensorIt->keys());
        for (auto colIt = sensorIt->cbegin(); colIt != sensorIt->cend(); ++colIt) {
            QVERIFY2(bitsEqual(sourceB[sensorIt.key()][colIt.key()].samples, colIt->samples),
                     qPrintable(sensorIt.key() + QLatin1Char('/') + colIt.key()));
        }
    }
    QVERIFY(sourceA == sourceB);
}

// Acceptance 5: source units and header attributes are preserved.
void PersistenceRoundTripTest::unitsAndHeaderAttributesPreserved()
{
    SessionData a;
    QVERIFY(readBuilder(awkwardFile(), a));
    SessionData b;
    QVERIFY(reload(exportBytes(a), b));

    QCOMPARE(b.sourceUnit("GNSS", "time"), QString());
    QCOMPARE(b.sourceUnit("GNSS", "lat"), QStringLiteral("deg"));
    QCOMPARE(b.sourceUnit("GNSS", "hMSL"), QStringLiteral("m"));
    QCOMPARE(b.sourceUnit("IMU", "time"), QStringLiteral("s"));
    QCOMPARE(b.sourceUnit("IMU", "wx"), QStringLiteral("deg/s"));
    QCOMPARE(b.sourceUnit("IMU", "ax"), QStringLiteral("g"));
    QCOMPARE(b.sourceUnit("FOO", "bar#1"), QStringLiteral("furlongs"));

    QCOMPARE(b.storedAttribute("FIRMWARE_VER"), QVariant(QStringLiteral("v2023.09.22")));
    QCOMPARE(b.storedAttribute("DEVICE_ID"), QVariant(QStringLiteral("test-device")));
    QCOMPARE(b.storedAttribute("SESSION_ID"), QVariant(QStringLiteral("awkward")));
    QCOMPARE(b.storedAttribute("CUSTOM_KEY"), QVariant(QStringLiteral("a=b, with, commas")));
    QVERIFY(b.hasAttribute("EMPTY_VAL"));
    QCOMPARE(b.storedAttribute("EMPTY_VAL"), QVariant(QString("")));
    QCOMPARE(b.storedAttribute("_DESCRIPTION"), QVariant(QStringLiteral("Perris, run 2, windy")));

    QCOMPARE(b.attributeKeys(), QStringList({"CUSTOM_KEY", "DEVICE_ID", "EMPTY_VAL", "FIRMWARE_VER",
                                             "SESSION_ID", "_DESCRIPTION"}));
    QCOMPARE(b.attributeKeys(), a.attributeKeys());
}

// Acceptance 5: SCHEMA_VER is absent if it was absent - also after the
// conversion layer has interpreted its absence for every column - and is
// written exactly as recorded when it was recorded.
void PersistenceRoundTripTest::schemaVerOnlyIfRecorded()
{
    SessionData a;
    QVERIFY(readBuilder(awkwardFile(), a));
    const SourceData source = a.sourceData();
    for (auto sensorIt = source.cbegin(); sensorIt != source.cend(); ++sensorIt) {
        for (auto colIt = sensorIt->cbegin(); colIt != sensorIt->cend(); ++colIt)
            a.getMeasurement(sensorIt.key(), colIt.key());
    }

    const QByteArray bytes = exportBytes(a);
    QVERIFY(!bytes.isEmpty());
    QVERIFY(!bytes.contains("SCHEMA_VER"));
    SessionData b;
    QVERIFY(reload(bytes, b));
    QVERIFY(!b.hasAttribute("SCHEMA_VER"));

    SessionData marked;
    QVERIFY(readBuilder(awkwardFile().var("SCHEMA_VER", "2"), marked));
    const QByteArray markedBytes = exportBytes(marked);
    QCOMPARE(markedBytes.count("SCHEMA_VER"), 1);
    QVERIFY(lines(markedBytes).contains(QByteArray("$VAR,SCHEMA_VER,2")));
    SessionData markedReloaded;
    QVERIFY(reload(markedBytes, markedReloaded));
    QCOMPARE(markedReloaded.storedAttribute("SCHEMA_VER"), QVariant(QStringLiteral("2")));
}

// Acceptance 5: effective values are unchanged by a save and reload.
void PersistenceRoundTripTest::effectiveValuesUnchanged()
{
    SessionData a;
    QVERIFY(readBuilder(awkwardFile(), a));
    SessionData b;
    QVERIFY(reload(exportBytes(a), b));

    const QVector<double> wxA = a.getMeasurement("IMU", "wx");
    const QVector<double> wxB = b.getMeasurement("IMU", "wx");
    QCOMPARE(wxA.size(), 2);
    QVERIFY(isNear(wxA.at(0), 71.68));
    QVERIFY(isNear(wxA.at(1), -143.36));
    QVERIFY(isNear(wxB.at(0), 71.68));
    QVERIFY(isNear(wxB.at(1), -143.36));
    QVERIFY(bitsEqual(wxA, wxB));

    const QVector<double> axA = a.getMeasurement("IMU", "ax");
    const QVector<double> axB = b.getMeasurement("IMU", "ax");
    QCOMPARE(axA.size(), 2);
    QVERIFY(isNear(axA.at(0), 3.2688833333333330));
    QVERIFY(sameBits(axA.at(0), axB.at(0)));

    QCOMPARE(b.effectiveUnit("IMU", "wx"), QStringLiteral("deg/s"));
    QCOMPARE(b.effectiveUnit("IMU", "ax"), QStringLiteral("m/s^2"));
    const SourceData source = a.sourceData();
    for (auto sensorIt = source.cbegin(); sensorIt != source.cend(); ++sensorIt) {
        for (auto colIt = sensorIt->cbegin(); colIt != sensorIt->cend(); ++colIt) {
            QCOMPARE(b.effectiveUnit(sensorIt.key(), colIt.key()), a.effectiveUnit(sensorIt.key(), colIt.key()));
            QVERIFY(bitsEqual(b.getMeasurement(sensorIt.key(), colIt.key()),
                              a.getMeasurement(sensorIt.key(), colIt.key())));
        }
    }

    const QList<DependencyKey> names = {
        DependencyKey::measurement("IMU", "wx"), DependencyKey::measurement("IMU", "ax"),
        DependencyKey::measurement("GNSS", "lat"), DependencyKey::measurement("GNSS", "hMSL"),
        DependencyKey::measurement("FOO", "bar#1"), DependencyKey::attribute("_DESCRIPTION")};
    QVERIFY(b.calculationEngine().verifyAgainstFresh(names).isEmpty());
}

// Acceptance 5: repeating the cycle changes nothing.
void PersistenceRoundTripTest::secondCycleIsByteIdentical()
{
    SessionData a;
    QVERIFY(readBuilder(awkwardFile(), a));

    const QByteArray b1 = exportBytes(a);
    QVERIFY(!b1.isEmpty());

    SessionData r1;
    QVERIFY(reload(b1, r1));
    const QByteArray b2 = exportBytes(r1);

    SessionData r2;
    QVERIFY(reload(b2, r2));
    const QByteArray b3 = exportBytes(r2);

    QCOMPARE(b2, b1);
    QCOMPARE(b3, b1);
}

// A file already in the exporter's canonical forms (numeric time, $VAR in
// exporter order, $UNIT present, standard column order) is reproduced
// byte for byte.
void PersistenceRoundTripTest::canonicalInputIsReproduced()
{
    const QByteArray canonical =
        "$FLYS,1\n"
        "$VAR,FIRMWARE_VER,v2023.09.22\n"
        "$VAR,DEVICE_ID,test-device\n"
        "$VAR,SESSION_ID,canon\n"
        "$VAR,CUSTOM_KEY,a=b, with, commas\n"
        "$VAR,EMPTY_VAL,\n"
        "$VAR,_DESCRIPTION,Perris, run 2, windy\n"
        "$COL,GNSS,time,lat,hMSL\n"
        "$UNIT,GNSS,,deg,m\n"
        "$COL,IMU,time,wx,ax\n"
        "$UNIT,IMU,s,deg/s,g\n"
        "$COL,FOO,bar#1,time\n"
        "$UNIT,FOO,furlongs,s\n"
        "$DATA\n"
        "$GNSS,1718900000.123,45.1234567,0.1\n"
        "$GNSS,1718900000.323,-0,1e-320\n"
        "$IMU,3,62.5,0.3333333333333333\n"
        "$IMU,4,-125,1.7976931348623157e+308\n"
        "$FOO,7,3\n";

    SessionData session;
    QVERIFY(reload(canonical, session));
    QCOMPARE(exportBytes(session), canonical);

    // The awkward file holds the same session (up to SESSION_ID) in device
    // forms - ISO time, another $VAR and column order - and saves to the same
    // canonical bytes.
    SessionData awkward;
    QVERIFY(readBuilder(awkwardFile(), awkward));
    awkward.setAttribute("SESSION_ID", QStringLiteral("canon"));
    QCOMPARE(exportBytes(awkward), canonical);
}

// Acceptance 5 for the attributes Viewer itself stores as numbers, booleans
// or dates: written exactly, reloaded as text with the same numeric value,
// stable from the first save on. (The baseline wrote _IMPORT_TIME as
// 1.7189e+09.)
void PersistenceRoundTripTest::typedAttributesRoundTrip()
{
    SessionData a;
    QVERIFY(readBuilder(awkwardFile(), a));
    a.setAttribute("_IMPORT_TIME", 1718900000.123);
    a.setAttribute("_GROUND_ELEV", 123.456);
    a.setAttribute("_WIND_N", 0.0);
    a.setAttribute("_EXIT_TIME", 1718900012.5);
    a.setAttribute("_JUMPER_MASS", 80);
    a.setAttribute("_FLAG", true);
    a.setAttribute("_WHEN", QDateTime(QDate(2024, 6, 20), QTime(16, 13, 20, 123), QTimeZone::utc()));

    const QByteArray bytes = exportBytes(a);
    const QList<QByteArray> fileLines = lines(bytes);
    QVERIFY(fileLines.contains(QByteArray("$VAR,_IMPORT_TIME,1718900000.123")));
    QVERIFY(fileLines.contains(QByteArray("$VAR,_GROUND_ELEV,123.456")));
    QVERIFY(fileLines.contains(QByteArray("$VAR,_WIND_N,0")));
    QVERIFY(fileLines.contains(QByteArray("$VAR,_EXIT_TIME,1718900012.5")));
    QVERIFY(fileLines.contains(QByteArray("$VAR,_JUMPER_MASS,80")));
    QVERIFY(fileLines.contains(QByteArray("$VAR,_FLAG,true")));
    QVERIFY(fileLines.contains(QByteArray("$VAR,_WHEN,2024-06-20T16:13:20.123Z")));

    SessionData b;
    QVERIFY(reload(bytes, b));
    QCOMPARE(b.storedAttribute("_IMPORT_TIME"), QVariant(QStringLiteral("1718900000.123")));
    QVERIFY(sameBits(b.storedAttribute("_IMPORT_TIME").toDouble(), 1718900000.123));
    QVERIFY(sameBits(b.storedAttribute("_GROUND_ELEV").toDouble(), 123.456));
    QVERIFY(sameBits(b.storedAttribute("_EXIT_TIME").toDouble(), 1718900012.5));

    // An effective value that uses a saved attribute: GNSS/z = hMSL - _GROUND_ELEV
    const QVector<double> zA = a.getMeasurement("GNSS", "z");
    const QVector<double> zB = b.getMeasurement("GNSS", "z");
    QCOMPARE(zA.size(), 2);
    QVERIFY(isNear(zA.at(0), -123.356));
    QVERIFY(isNear(zA.at(1), -123.456));
    QVERIFY(bitsEqual(zA, zB));
    QCOMPARE(b.getAttribute("_EXIT_TIME").toDouble(), a.getAttribute("_EXIT_TIME").toDouble());

    QCOMPARE(exportBytes(b), bytes);
}

// Acceptance 5: the result does not depend on the state of any cache. Also
// Task 5.3: a save neither computes nor creates anything in the engine.
void PersistenceRoundTripTest::warmAndColdCachesSameFile()
{
    SessionData cold;
    SessionData warm;
    QVERIFY(readBuilder(awkwardFile(), cold));
    QVERIFY(readBuilder(awkwardFile(), warm));

    // Warm everything a consumer could have read
    const SourceData source = warm.sourceData();
    for (auto sensorIt = source.cbegin(); sensorIt != source.cend(); ++sensorIt) {
        for (auto colIt = sensorIt->cbegin(); colIt != sensorIt->cend(); ++colIt) {
            warm.getMeasurement(sensorIt.key(), colIt.key());
            warm.effectiveUnit(sensorIt.key(), colIt.key());
        }
    }
    warm.getMeasurement("IMU", "wTotal");
    warm.getMeasurement("GNSS", "z");
    warm.getAttribute("_START_TIME");
    warm.getAttribute("_EXIT_TIME");
    warm.setAttribute("_M", 3.5);
    cold.setAttribute("_M", 3.5);
    QVERIFY(warm.getAttribute(SessionData::interpolationKey("_M", "IMU", "time", "wx")).isValid());

    const int runs = warm.calculationEngine().totalRunCount();
    const int nodes = warm.calculationEngine().cachedNodeCount();
    QVERIFY(runs > 0);
    QVERIFY(nodes > 0);

    const QByteArray coldBytes = exportBytes(cold);
    const QByteArray warmBytes = exportBytes(warm);
    QVERIFY(!coldBytes.isEmpty());
    QCOMPARE(warmBytes, coldBytes);

    // The same through exportSession
    const QString path = tempFile(QStringLiteral("warm"));
    QVERIFY(DataExporter::exportSession(path, warm));
    QCOMPARE(readFileBytes(path), coldBytes);
    const QString coldPath = tempFile(QStringLiteral("cold"));
    QVERIFY(DataExporter::exportSession(coldPath, cold));
    QCOMPARE(readFileBytes(coldPath), coldBytes);

    // Exporting left the warm engine alone and did nothing in the cold one
    QCOMPARE(warm.calculationEngine().totalRunCount(), runs);
    QCOMPARE(warm.calculationEngine().cachedNodeCount(), nodes);
    QCOMPARE(cold.calculationEngine().totalRunCount(), 0);
    QCOMPARE(cold.calculationEngine().cachedNodeCount(), 0);

    // Source values under source units; no effective value leaked into the file
    QVERIFY(coldBytes.contains("62.5"));
    QVERIFY(coldBytes.contains(",g\n"));
    QVERIFY(!coldBytes.contains("71.6"));
    QVERIFY(!coldBytes.contains("9.80665"));
    QVERIFY(!coldBytes.contains("wTotal"));
    QVERIFY(!coldBytes.contains("m/s^2"));
}

// Spec 9.2: a non-finite sample survives save and reload in place; it is
// never written as zero and never dropped. The same for a double attribute.
void PersistenceRoundTripTest::nonFiniteRoundTrip()
{
    SessionData session;
    session.setSourceMeasurement("X", "time", {0.0, 1.0, 2.0, 3.0, 4.0}, "s");
    session.setSourceMeasurement("X", "v", {1.0, kNaN, kInf, -kInf, 2.0}, "u");
    session.setAttribute("_GROUND_ELEV", kNaN);

    const QByteArray expected =
        "$FLYS,1\n"
        "$VAR,_GROUND_ELEV,nan\n"
        "$COL,X,time,v\n"
        "$UNIT,X,s,u\n"
        "$DATA\n"
        "$X,0,1\n"
        "$X,1,nan\n"
        "$X,2,inf\n"
        "$X,3,-inf\n"
        "$X,4,2\n";
    const QByteArray bytes = exportBytes(session);
    QCOMPARE(bytes, expected);
    QVERIFY(!bytes.contains(",0\n"));

    WarningCollector warnings;
    SessionData reloaded;
    QVERIFY(reload(bytes, reloaded));
    QCOMPARE(warnings.count(QStringLiteral("skipped")), 0);

    const QVector<double> v = reloaded.sourceMeasurement("X", "v");
    QCOMPARE(v.size(), 5);
    QVERIFY(sameBits(v.at(0), 1.0));
    QVERIFY(std::isnan(v.at(1)));
    QVERIFY(v.at(2) == kInf);
    QVERIFY(v.at(3) == -kInf);
    QVERIFY(sameBits(v.at(4), 2.0));
    QVERIFY(bitsEqual(reloaded.sourceMeasurement("X", "time"), {0.0, 1.0, 2.0, 3.0, 4.0}));

    QCOMPARE(reloaded.storedAttribute("_GROUND_ELEV"), QVariant(QStringLiteral("nan")));
    QVERIFY(std::isnan(reloaded.storedAttribute("_GROUND_ELEV").toDouble()));

    QCOMPARE(exportBytes(reloaded), bytes);
}

// A sensor whose columns have different lengths cannot be expressed in the
// row format. The save fails up front - no out-of-range read, nothing
// written, the previous file intact.
void PersistenceRoundTripTest::raggedSensorIsRejected()
{
    SessionData session;
    session.setAttribute("SESSION_ID", QStringLiteral("ragged"));
    session.setSourceMeasurement("X", "time", {0.0, 1.0, 2.0}, "s");
    session.setSourceMeasurement("X", "v", {1.0, 2.0}, "u");

    const QString path = tempFile(QStringLiteral("ragged"));
    QVERIFY(writeFile(path, "keep"));

    QString error;
    QVERIFY(!DataExporter::exportSession(path, session, &error));
    QCOMPARE(error, QStringLiteral("Sensor 'X' has columns of unequal length (time: 3, v: 2)"));
    QCOMPARE(readFileBytes(path), QByteArray("keep"));

    QString bytesError;
    QVERIFY(!DataExporter::toBytes(session, &bytesError).has_value());
    QCOMPARE(bytesError, error);

    // The longer column second: the first column and the first that differs
    SessionData other;
    other.setSourceMeasurement("GNSS", "time", {0.0}, "");
    other.setSourceMeasurement("GNSS", "lat", {0.0}, "deg");
    other.setSourceMeasurement("GNSS", "hMSL", {0.0, 1.0, 2.0}, "m");
    QVERIFY(!DataExporter::exportSession(path, other, &error));
    QCOMPARE(error, QStringLiteral("Sensor 'GNSS' has columns of unequal length (time: 1, hMSL: 3)"));
    QCOMPARE(readFileBytes(path), QByteArray("keep"));
}

// A header-only recording ($COL, $DATA, no rows) is valid.
void PersistenceRoundTripTest::headerOnlySensor()
{
    Fs2FileBuilder file;
    file.var("SESSION_ID", "header-only")
        .var("DEVICE_ID", "test-device")
        .sensor("IMU", {"time", "wx"}, {});

    SessionData session;
    QVERIFY(readBuilder(file, session));

    const QByteArray expected =
        "$FLYS,1\n"
        "$VAR,DEVICE_ID,test-device\n"
        "$VAR,SESSION_ID,header-only\n"
        "$COL,IMU,time,wx\n"
        "$UNIT,IMU,,\n"
        "$DATA\n";
    const QByteArray bytes = exportBytes(session);
    QCOMPARE(bytes, expected);

    SessionData reloaded;
    QVERIFY(reload(bytes, reloaded));
    QCOMPARE(reloaded.measurementKeys("IMU"), QStringList({"time", "wx"}));
    QVERIFY(reloaded.sourceMeasurement("IMU", "time").isEmpty());
    QVERIFY(reloaded.sourceMeasurement("IMU", "wx").isEmpty());
    QCOMPARE(exportBytes(reloaded), bytes);
}

// Text a line format cannot hold: a line break in a value becomes a space; an
// attribute with an unwritable key is skipped with a warning; unwritable
// sensor / column / unit text fails the save and leaves the old file intact.
void PersistenceRoundTripTest::unrepresentableText()
{
    SessionData session;
    session.setAttribute("SESSION_ID", QStringLiteral("odd"));
    session.setAttribute("_DESCRIPTION", QStringLiteral("a\nb"));
    session.setAttribute("BAD,KEY", QStringLiteral("x"));
    session.setAttribute("_INVALID", QVariant());
    session.setSourceMeasurement("X", "time", {1.0}, "s");

    {
        WarningCollector warnings;
        const QByteArray bytes = exportBytes(session);
        const QByteArray expected =
            "$FLYS,1\n"
            "$VAR,SESSION_ID,odd\n"
            "$VAR,_DESCRIPTION,a b\n"
            "$COL,X,time\n"
            "$UNIT,X,s\n"
            "$DATA\n"
            "$X,1\n";
        QCOMPARE(bytes, expected);
        QCOMPARE(warnings.count(QStringLiteral("attribute 'BAD,KEY' cannot be written")), 1);
    }

    const QString path = tempFile(QStringLiteral("odd"));
    QVERIFY(writeFile(path, "keep"));
    QString error;

    SessionData badLabel = session;
    badLabel.setSourceMeasurement("X", "a,b", {1.0}, "u");
    QVERIFY(!DataExporter::exportSession(path, badLabel, &error));
    QCOMPARE(error, QStringLiteral("Sensor 'X': name/column/unit text cannot be written ('a,b')"));
    QCOMPARE(readFileBytes(path), QByteArray("keep"));

    SessionData badUnit = session;
    badUnit.setSourceMeasurement("X", "v", {1.0}, "m\ns");
    QVERIFY(!DataExporter::exportSession(path, badUnit, &error));
    QCOMPARE(error, QStringLiteral("Sensor 'X': name/column/unit text cannot be written ('m\ns')"));
    QCOMPARE(readFileBytes(path), QByteArray("keep"));

    SessionData badSensor = session;
    badSensor.setSourceMeasurement("Y,Z", "time", {1.0}, "s");
    QVERIFY(!DataExporter::exportSession(path, badSensor, &error));
    QCOMPARE(error, QStringLiteral("Sensor 'Y,Z': name/column/unit text cannot be written ('Y,Z')"));
    QCOMPARE(readFileBytes(path), QByteArray("keep"));
}

// Acceptance 6: a logbook file written by a released version - physical
// units, no SCHEMA_VER - is corrected once on read, and saving it does not
// rescale, relabel, or stamp it: the bytes stay what they were.
void PersistenceRoundTripTest::releasedLogbookSaveKeepsBytes()
{
    LogbookManager &logbook = LogbookManager::instance();
    logbook.initialize();

    const QByteArray released = releasedFile(true);
    const QString path = installReleasedFile(released);
    QVERIFY(!path.isEmpty());

    std::optional<SessionData> loaded = logbook.loadSession(QStringLiteral("rel"));
    QVERIFY(loaded.has_value());
    QVERIFY(isNear(loaded->getMeasurement("IMU", "wx").value(0), 71.68));
    QCOMPARE(loaded->getMeasurement("IMU", "ax"), QVector<double>({9.80665}));
    QCOMPARE(loaded->getMeasurement("MAG", "x"), QVector<double>({0.25}));

    QVERIFY2(logbook.saveSession(*loaded), qPrintable(logbook.lastSaveError()));

    const QByteArray saved = readFileBytes(path);
    QCOMPARE(saved, released);
    // Spelled out: no rescale, no relabel, no stamp, no "repair" of old text
    QVERIFY(saved.contains("$IMU,3,62.5,-125,0,9.80665,40\n"));
    QVERIFY(saved.contains("$MAG,3,0.25\n"));
    QVERIFY(saved.contains("$UNIT,IMU,s,deg/s,deg/s,deg/s,m/s^2,degC\n"));
    QVERIFY(saved.contains("$UNIT,MAG,s,T\n"));
    QVERIFY(!saved.contains("SCHEMA_VER"));
    QVERIFY(saved.contains("$VAR,_IMPORT_TIME,1.7189e+09\n"));

    // Corrected once - not a second time after the save
    std::optional<SessionData> again = logbook.loadSession(QStringLiteral("rel"));
    QVERIFY(again.has_value());
    QVERIFY(isNear(again->getMeasurement("IMU", "wx").value(0), 71.68));
    QVERIFY(!isNear(again->getMeasurement("IMU", "wx").value(0), 82.2));
    QVERIFY(!again->hasAttribute("SCHEMA_VER"));
}

// Acceptance 6, the fine print: byte identity is guaranteed from the first
// file THIS version writes. A number a released version wrote re-saves to the
// same VALUE but not always to the same text: released versions wrote 0.0001
// (15 significant digits, %g rules), the shortest round-trip form of that
// double is 1e-04. Values, units and attributes are what must not change.
void PersistenceRoundTripTest::releasedValueWithDifferentText()
{
    LogbookManager &logbook = LogbookManager::instance();
    logbook.initialize();

    const QString path = installReleasedFile(releasedFile(true, "0.0001"));
    QVERIFY(!path.isEmpty());

    std::optional<SessionData> loaded = logbook.loadSession(QStringLiteral("rel"));
    QVERIFY(loaded.has_value());
    QVERIFY(logbook.saveSession(*loaded));

    const QByteArray saved = readFileBytes(path);
    QCOMPARE(saved, releasedFile(true, "1e-04"));

    std::optional<SessionData> again = logbook.loadSession(QStringLiteral("rel"));
    QVERIFY(again.has_value());
    QVERIFY(bitsEqual(again->sourceMeasurement("MAG", "x"), {0.0001}));
    QCOMPARE(again->sourceUnit("MAG", "x"), QStringLiteral("T"));
    QCOMPARE(again->getMeasurement("MAG", "x"), QVector<double>({0.0001}));   // not rescaled as if gauss
    QVERIFY(loaded->sourceData() == again->sourceData());

    // From here on the bytes are stable
    QVERIFY(logbook.saveSession(*again));
    QCOMPARE(readFileBytes(path), saved);
}

// Acceptance 6 / the loadSession backfill: a released file older than the
// per-session mass / area / wind attributes gains exactly those four lines on
// its next save and nothing else changes.
// Phase 6 owns the backfill policy; this pins that it is additive and idempotent.
void PersistenceRoundTripTest::releasedLogbookBackfillIsAdditive()
{
    PreferencesManager &prefs = PreferencesManager::instance();
    prefs.setValue(PreferenceKeys::AeroMass, 1.0);
    prefs.setValue(PreferenceKeys::AeroArea, 1.0);

    LogbookManager &logbook = LogbookManager::instance();
    logbook.initialize();

    const QByteArray released = releasedFile(false);
    const QString path = installReleasedFile(released);
    QVERIFY(!path.isEmpty());

    std::optional<SessionData> loaded = logbook.loadSession(QStringLiteral("rel"));
    QVERIFY(loaded.has_value());
    QVERIFY2(logbook.saveSession(*loaded), qPrintable(logbook.lastSaveError()));

    const QByteArray saved = readFileBytes(path);
    const QList<QByteArray> added = {
        "$VAR,_JUMPER_MASS,1", "$VAR,_PLANFORM_AREA,1", "$VAR,_WIND_E,0", "$VAR,_WIND_N,0"};

    // Exactly the four lines were added ...
    QList<QByteArray> savedLines = lines(saved);
    for (const QByteArray &line : added) {
        QCOMPARE(savedLines.count(line), 1);
        savedLines.removeOne(line);
    }
    // ... and without them the file is the released file, line for line.
    QCOMPARE(savedLines, lines(released));
    QCOMPARE(saved, releasedFile(true).replace("_JUMPER_MASS,80", "_JUMPER_MASS,1")
                                      .replace("_PLANFORM_AREA,2", "_PLANFORM_AREA,1"));

    // Idempotent: a further load + save changes nothing
    std::optional<SessionData> again = logbook.loadSession(QStringLiteral("rel"));
    QVERIFY(again.has_value());
    QVERIFY(logbook.saveSession(*again));
    QCOMPARE(readFileBytes(path), saved);
}

// Acceptance 5 through the logbook: import, save, restart, load, save.
void PersistenceRoundTripTest::logbookSaveReloadCycle()
{
    TestEnvironment &env = TestEnvironment::instance();
    LogbookManager &logbook = LogbookManager::instance();
    logbook.initialize();

    // importFile, so that the creation defaults (import time as a double,
    // mass, area, wind) exist and loadSession has nothing to backfill.
    const QString source = tempFile(QStringLiteral("awkward"));
    QVERIFY(awkwardFile().write(source));
    DataImporter importer;
    SessionData imported;
    QVERIFY2(importer.importFile(source, imported), qPrintable(importer.getLastError()));
    QCOMPARE(imported.storedAttribute("_IMPORT_TIME").typeId(), int(QMetaType::Double));

    QVERIFY2(logbook.saveSession(imported), qPrintable(logbook.lastSaveError()));
    QVERIFY(logbook.flushIndex());

    const QStringList files = sessionCsvFiles();
    QCOMPARE(files.size(), 1);
    const QString path = env.sessionsDir() + QLatin1Char('/') + files.first();
    const QByteArray firstBytes = readFileBytes(path);
    QVERIFY(firstBytes.startsWith("$FLYS,1\n"));

    env.reopenLogbook();
    logbook.initialize();
    std::optional<SessionData> loaded = logbook.loadSession(QStringLiteral("awkward"));
    QVERIFY(loaded.has_value());

    // Bit-identical source, and the import time survived exactly
    const SourceData before = imported.sourceData();
    const SourceData after = loaded->sourceData();
    QVERIFY(before == after);
    QVERIFY(std::signbit(loaded->sourceMeasurement("GNSS", "lat").at(1)));
    QVERIFY(bitsEqual(loaded->sourceMeasurement("GNSS", "time"), {1718900000.123, 1718900000.323}));
    QVERIFY(sameBits(loaded->storedAttribute("_IMPORT_TIME").toDouble(),
                     imported.storedAttribute("_IMPORT_TIME").toDouble()));
    QCOMPARE(loaded->attributeKeys(), imported.attributeKeys());

    QVERIFY(logbook.saveSession(*loaded));
    QCOMPARE(readFileBytes(path), firstBytes);
    QCOMPARE(sessionCsvFiles(), files);
}

FLYSIGHT_TEST_MAIN(PersistenceRoundTripTest)
#include "tst_persistence_roundtrip.moc"
