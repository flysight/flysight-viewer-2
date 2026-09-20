// The importer: what it publishes (samples, unit text and header attributes
// exactly as recorded, nothing stamped), what it rejects (unsupported
// SCHEMA_VER, structural header errors) without touching the target session,
// and what it tolerates (malformed data rows). Every expectation is a literal.
//
// Acceptance 3 (a file declaring SCHEMA_VER 3 / abc is rejected and the target
// session is unmodified) is demonstrated here at the importer level.
//
// Parsing versus creation: parseFile() carries nothing the file did
// not say; applyCreationDefaults() is the one writer of import-time defaults
// and only fills keys that are absent.

#include <QtTest>

#include <cmath>
#include <limits>

#include <QDir>

#include "csvformat.h"
#include "dataimporter.h"
#include "engine/calculationengine.h"
#include "fixturebuilder.h"
#include "parsedfile.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "sessiondata.h"
#include "testenvironment.h"
#include "testmain.h"
#include "testutil.h"

using namespace FlySight;
using namespace FlySightTest;

namespace {

// Writes the bytes to a file in a fresh temp dir and returns its path.
QString writeTemp(const QByteArray &bytes, const QString &fileName = QStringLiteral("file.csv"))
{
    const QString path = TestEnvironment::instance().newTempDir(QStringLiteral("importer"))
                         + QLatin1Char('/') + fileName;
    if (!writeFile(path, bytes))
        qFatal("could not write %s", qPrintable(path));
    return path;
}

bool nothingPublished(const SessionData &session)
{
    return session.attributeKeys().isEmpty() && session.sensorKeys().isEmpty();
}

} // namespace

class ImporterTest : public QObject {
    Q_OBJECT

private slots:
    void init();

    // SCHEMA_VER validation (acceptance 3)
    void rejectsUnsupportedSchema_data();
    void rejectsUnsupportedSchema();
    void failedImportLeavesTargetUntouched();
    void acceptsSchema1And2();
    void neverStampsSchema();
    void lastErrorClearedAtEntry();

    // header grammar
    void varValueKeepsCommas();
    void equalDuplicateVarCollapses();
    void structuralErrors_data();
    void structuralErrors();
    void missingDataSection();
    void headerOnlyWithDataMarker();
    void missingUnitsAreEmpty();
    void unknownHeaderLinesIgnored();

    // data rows
    void malformedRowsAreSkipped();
    void cleanFileIsSilent();
    void isoTimestamps();
    void storesAsRecorded();
    void nonFiniteTokens();

    // formats
    void fs1StillImports();
    void fs1WithoutUnitLine();
    void customColumnsAndSensors();
    void crlfLineEndings();
    void existingErrorsUnchanged();

    // parse result versus session creation
    void parseFileCarriesNoDefaults();
    void parseFileSynthesizesMatchId();
    void parseFailureLeavesResultEmpty();
    void creationDefaultsOnlyFillAbsent();
    void creationDefaultsDeviceId();
    void fixedGroundElevationOnlyInFixedMode();
    void peekHeaderAttributeReadsNoData();
};

void ImporterTest::init()
{
    TestEnvironment::instance().resetPreferencesToDefaults();
}

// ─────────────────────────────── SCHEMA_VER validation

void ImporterTest::rejectsUnsupportedSchema_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<QString>("error");

    QTest::newRow("3") << Fixtures::sensorFile().var("SCHEMA_VER", "3").toBytes()
                       << "Unsupported SCHEMA_VER '3' (supported: 1, 2)";
    QTest::newRow("abc") << Fixtures::sensorFile().var("SCHEMA_VER", "abc").toBytes()
                         << "Unsupported SCHEMA_VER 'abc' (supported: 1, 2)";
    QTest::newRow("empty value") << Fixtures::sensorFile().var("SCHEMA_VER", "").toBytes()
                                 << "Unsupported SCHEMA_VER '' (supported: 1, 2)";
    QTest::newRow("no value") << Fixtures::sensorFile().rawHeaderLine("$VAR,SCHEMA_VER").toBytes()
                              << "Unsupported SCHEMA_VER '' (supported: 1, 2)";
    QTest::newRow("2.0") << Fixtures::sensorFile().var("SCHEMA_VER", "2.0").toBytes()
                         << "Unsupported SCHEMA_VER '2.0' (supported: 1, 2)";
    QTest::newRow("track file") << Fixtures::trackFile().var("SCHEMA_VER", "3").toBytes()
                                << "Unsupported SCHEMA_VER '3' (supported: 1, 2)";
}

// Acceptance 3
void ImporterTest::rejectsUnsupportedSchema()
{
    QFETCH(QByteArray, bytes);
    QFETCH(QString, error);

    const QString path = writeTemp(bytes);

    DataImporter importer;
    SessionData session;
    QVERIFY(!importer.importFile(path, session));
    QCOMPARE(importer.getLastError(), error);
    QVERIFY(nothingPublished(session));

    DataImporter reader;
    SessionData readTarget;
    QVERIFY(!reader.readFile(path, readTarget));
    QCOMPARE(reader.getLastError(), error);
    QVERIFY(nothingPublished(readTarget));
}

// Acceptance 3
void ImporterTest::failedImportLeavesTargetUntouched()
{
    SessionData target;
    target.setAttribute("SESSION_ID", QStringLiteral("test-session"));
    target.setAttribute("_DESCRIPTION", QStringLiteral("edited"));
    target.setSourceMeasurement("IMU", "wx", {1.0, 2.0}, "deg/s");

    const QStringList keysBefore = target.attributeKeys();
    const SourceData sourceBefore = target.sourceData();

    const QString path = writeTemp(Fixtures::sensorFile().var("SCHEMA_VER", "3").toBytes());
    DataImporter importer;
    QVERIFY(!importer.importFile(path, target));
    QCOMPARE(importer.getLastError(), QStringLiteral("Unsupported SCHEMA_VER '3' (supported: 1, 2)"));

    QCOMPARE(target.attributeKeys(), keysBefore);
    QCOMPARE(target.attributeKeys(), QStringList({"SESSION_ID", "_DESCRIPTION"}));
    QCOMPARE(target.getAttribute("_DESCRIPTION").toString(), QStringLiteral("edited"));
    QVERIFY(target.sourceData() == sourceBefore);
    QCOMPARE(target.sensorKeys(), QStringList({"IMU"}));
    QCOMPARE(target.sourceMeasurement("IMU", "wx"), QVector<double>({1.0, 2.0}));
    QCOMPARE(target.sourceUnit("IMU", "wx"), QStringLiteral("deg/s"));

    // applyCreationDefaults did not run
    QVERIFY(!target.hasAttribute("_IMPORT_TIME"));
    QVERIFY(!target.hasAttribute("SCHEMA_VER"));

    // A structural error leaves it untouched just the same.
    const QString broken = writeTemp("$FLYS,1\n$VAR,SESSION_ID,test-session\n$COL,IMU,time,wx,wx\n$DATA\n");
    QVERIFY(!importer.importFile(broken, target));
    QCOMPARE(target.attributeKeys(), keysBefore);
    QVERIFY(target.sourceData() == sourceBefore);
}

void ImporterTest::acceptsSchema1And2()
{
    {
        DataImporter importer;
        SessionData session;
        QVERIFY2(importer.importFile(writeTemp(Fixtures::sensorFile().var("SCHEMA_VER", "2").toBytes()), session),
                 qPrintable(importer.getLastError()));
        QCOMPARE(session.getAttribute("SCHEMA_VER").userType(), int(QMetaType::QString));
        QCOMPARE(session.getAttribute("SCHEMA_VER").toString(), QStringLiteral("2"));
    }
    {
        DataImporter importer;
        SessionData session;
        QVERIFY2(importer.importFile(writeTemp(Fixtures::sensorFile().var("SCHEMA_VER", "1").toBytes()), session),
                 qPrintable(importer.getLastError()));
        QCOMPARE(session.getAttribute("SCHEMA_VER").userType(), int(QMetaType::QString));
        QCOMPARE(session.getAttribute("SCHEMA_VER").toString(), QStringLiteral("1"));
    }
    {
        // The recorded text is kept as it is, even when validation had to trim it.
        DataImporter importer;
        SessionData session;
        QVERIFY(importer.importFile(writeTemp(Fixtures::sensorFile().var("SCHEMA_VER", " 2").toBytes()), session));
        QCOMPARE(session.getAttribute("SCHEMA_VER").toString(), QStringLiteral(" 2"));
    }
}

// Acceptance 1 (the SCHEMA_VER part): an unmarked file yields a session without the attribute.
void ImporterTest::neverStampsSchema()
{
    {
        DataImporter importer;
        SessionData session;
        QVERIFY(importer.importFile(writeTemp(Fixtures::sensorFile().toBytes()), session));
        QVERIFY(!session.hasAttribute("SCHEMA_VER"));
        QVERIFY(!session.attributeKeys().contains(QStringLiteral("SCHEMA_VER")));
    }
    {
        DataImporter importer;
        SessionData session;
        QVERIFY(importer.importFile(writeTemp(Fixtures::trackFile().toBytes()), session));
        QVERIFY(!session.hasAttribute("SCHEMA_VER"));
    }
    {
        Fs1FileBuilder fs1;
        fs1.columns({"time", "lat", "lon", "hMSL"})
           .units({"", "(deg)", "(deg)", "(m)"})
           .row("2024-01-01T12:00:00.00Z,45.5,-73.25,4000.5");
        const QString path = writeTemp(fs1.toBytes());

        WarningCapture quiet;   // FS1 has no DEVICE_ID: the FLYSIGHT.TXT search warns
        DataImporter importer;
        SessionData session;
        QVERIFY(importer.importFile(path, session));
        QVERIFY(!session.hasAttribute("SCHEMA_VER"));
    }
}

void ImporterTest::lastErrorClearedAtEntry()
{
    DataImporter importer;

    SessionData bad;
    QVERIFY(!importer.importFile(writeTemp(Fixtures::sensorFile().var("SCHEMA_VER", "3").toBytes()), bad));
    QVERIFY(!importer.getLastError().isEmpty());

    SessionData good;
    QVERIFY(importer.importFile(writeTemp(Fixtures::sensorFile().toBytes()), good));
    QVERIFY(importer.getLastError().isEmpty());

    SessionData bad2;
    QVERIFY(!importer.readFile(writeTemp("hello\n"), bad2));
    QVERIFY(!importer.getLastError().isEmpty());

    SessionData good2;
    QVERIFY(importer.readFile(writeTemp(Fixtures::trackFile().toBytes()), good2));
    QVERIFY(importer.getLastError().isEmpty());
}

// ─────────────────────────────── header grammar

void ImporterTest::varValueKeepsCommas()
{
    Fs2FileBuilder file = Fixtures::sensorFile();
    file.rawHeaderLine("$VAR,_DESCRIPTION,Perris, run 2, windy")
        .rawHeaderLine("$VAR,CUSTOM_KEY,a=b")
        .rawHeaderLine("$VAR,EMPTY_VAL,")
        .rawHeaderLine("$VAR,NO_VAL")
        .rawHeaderLine("$VAR,SPACED,  padded  ");

    DataImporter importer;
    SessionData session;
    QVERIFY2(importer.readFile(writeTemp(file.toBytes()), session), qPrintable(importer.getLastError()));

    QCOMPARE(session.getAttribute("_DESCRIPTION").toString(), QStringLiteral("Perris, run 2, windy"));
    QCOMPARE(session.getAttribute("CUSTOM_KEY").toString(), QStringLiteral("a=b"));
    QCOMPARE(session.getAttribute("SPACED").toString(), QStringLiteral("  padded  "));

    QVERIFY(session.hasAttribute("EMPTY_VAL"));
    QCOMPARE(session.getAttribute("EMPTY_VAL").userType(), int(QMetaType::QString));
    QCOMPARE(session.getAttribute("EMPTY_VAL").toString(), QString());
    QVERIFY(session.hasAttribute("NO_VAL"));
    QCOMPARE(session.getAttribute("NO_VAL").toString(), QString());

    QCOMPARE(session.attributeKeys(),
             QStringList({"CUSTOM_KEY", "DEVICE_ID", "EMPTY_VAL", "FIRMWARE_VER", "NO_VAL",
                          "SESSION_ID", "SPACED", "_DESCRIPTION"}));
}

void ImporterTest::equalDuplicateVarCollapses()
{
    const QString path = writeTemp("$FLYS,1\n$VAR,A,1, x\n$VAR,A,1, x\n$DATA\n");
    DataImporter importer;
    SessionData session;
    QVERIFY2(importer.readFile(path, session), qPrintable(importer.getLastError()));
    QCOMPARE(session.attributeKeys(), QStringList({"A"}));
    QCOMPARE(session.getAttribute("A").toString(), QStringLiteral("1, x"));
}

void ImporterTest::structuralErrors_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<QString>("error");

    QTest::newRow("$VAR empty name")
        << QByteArray("$FLYS,1\n$VAR,,x\n$DATA\n") << "Line 2: $VAR with empty name";
    QTest::newRow("$VAR alone")
        << QByteArray("$FLYS,1\n$VAR,A,1\n$VAR\n$DATA\n") << "Line 3: $VAR with empty name";
    QTest::newRow("$VAR conflict")
        << QByteArray("$FLYS,1\n$VAR,A,1\n$VAR,B,1\n$VAR,A,2\n$DATA\n")
        << "Line 4: conflicting values for $VAR A";
    QTest::newRow("$COL alone")
        << QByteArray("$FLYS,1\n$COL\n$DATA\n") << "Line 2: $COL without sensor name";
    QTest::newRow("$COL empty sensor")
        << QByteArray("$FLYS,1\n$COL,,time,wx\n$DATA\n") << "Line 2: $COL without sensor name";
    QTest::newRow("$COL no columns")
        << QByteArray("$FLYS,1\n$COL,IMU\n$DATA\n") << "Line 2: $COL IMU has no columns";
    QTest::newRow("$COL empty column")
        << QByteArray("$FLYS,1\n$COL,IMU,time,,wx\n$DATA\n") << "Line 2: $COL IMU has an empty column name";
    QTest::newRow("$COL trailing comma")
        << QByteArray("$FLYS,1\n$COL,IMU,time,wx,\n$DATA\n") << "Line 2: $COL IMU has an empty column name";
    QTest::newRow("$COL repeated column")
        << QByteArray("$FLYS,1\n$COL,IMU,time,wx,wx\n$DATA\n") << "Line 2: $COL IMU repeats column 'wx'";
    QTest::newRow("$COL twice")
        << QByteArray("$FLYS,1\n$COL,IMU,time,wx\n$COL,IMU,time,wy\n$DATA\n")
        << "Line 3: duplicate $COL for sensor IMU";
    QTest::newRow("$UNIT before $COL")
        << QByteArray("$FLYS,1\n$UNIT,IMU,s,deg/s\n$COL,IMU,time,wx\n$DATA\n")
        << "Line 2: $UNIT for unknown sensor IMU";
    QTest::newRow("$UNIT too long")
        << QByteArray("$FLYS,1\n$COL,IMU,time,wx\n$UNIT,IMU,s,deg/s,extra\n$DATA\n")
        << "Line 3: $UNIT IMU has more units than columns";
    QTest::newRow("$UNIT twice")
        << QByteArray("$FLYS,1\n$COL,IMU,time,wx\n$UNIT,IMU,s,deg/s\n$UNIT,IMU,s,deg/s\n$DATA\n")
        << "Line 4: duplicate $UNIT for sensor IMU";
    QTest::newRow("CRLF line numbers")
        << QByteArray("$FLYS,1\r\n$VAR,A,1\r\n\r\n$COL,IMU\r\n$DATA\r\n") << "Line 4: $COL IMU has no columns";

    QTest::newRow("FS1 empty column")
        << QByteArray("time,lat,lon,hMSL,,velN\n,(deg),(deg),(m),,(m/s)\n") << "Line 1: column header has an empty name";
    QTest::newRow("FS1 repeated column")
        << QByteArray("time,lat,lon,hMSL,lat\n,(deg),(deg),(m),(deg)\n") << "Line 1: column header repeats 'lat'";
    QTest::newRow("FS1 too many units")
        << QByteArray("time,lat,lon,hMSL\n,(deg),(deg),(m),(m/s)\n") << "Line 2: unit line has more units than columns";
}

void ImporterTest::structuralErrors()
{
    QFETCH(QByteArray, bytes);
    QFETCH(QString, error);

    DataImporter importer;
    SessionData session;
    QVERIFY(!importer.importFile(writeTemp(bytes), session));
    QCOMPARE(importer.getLastError(), error);
    QVERIFY(nothingPublished(session));
}

void ImporterTest::missingDataSection()
{
    const QString path = writeTemp("$FLYS,1\n$VAR,SESSION_ID,x\n$COL,IMU,time,wx\n$UNIT,IMU,s,deg/s\n");
    DataImporter importer;
    SessionData session;
    QVERIFY(!importer.importFile(path, session));
    QCOMPARE(importer.getLastError(), QStringLiteral("Missing $DATA section"));
    QVERIFY(nothingPublished(session));
}

void ImporterTest::headerOnlyWithDataMarker()
{
    // Header-only recordings exist; zero rows is not an error.
    const QString path = writeTemp("$FLYS,1\n$VAR,SESSION_ID,x\n$COL,IMU,time,wx\n$UNIT,IMU,s,deg/s\n$DATA\n");
    DataImporter importer;
    SessionData session;
    QVERIFY2(importer.readFile(path, session), qPrintable(importer.getLastError()));

    QCOMPARE(session.sensorKeys(), QStringList({"IMU"}));
    QCOMPARE(session.measurementKeys("IMU"), QStringList({"time", "wx"}));
    QVERIFY(session.hasMeasurement("IMU", "wx"));
    QVERIFY(session.hasSourceMeasurement("IMU", "wx"));
    QVERIFY(session.sourceMeasurement("IMU", "wx").isEmpty());
    QCOMPARE(session.sourceUnit("IMU", "wx"), QStringLiteral("deg/s"));
}

void ImporterTest::missingUnitsAreEmpty()
{
    const QString path = writeTemp("$FLYS,1\n"
                                   "$COL,A,time,x,y\n"
                                   "$UNIT,A,s\n"          // fewer units than columns
                                   "$COL,B,time,x\n"      // no $UNIT line at all
                                   "$DATA\n"
                                   "$A,1,2,3\n"
                                   "$B,4,5\n");
    DataImporter importer;
    SessionData session;
    QVERIFY2(importer.readFile(path, session), qPrintable(importer.getLastError()));

    QCOMPARE(session.sourceUnit("A", "time"), QStringLiteral("s"));
    QCOMPARE(session.sourceUnit("A", "x"), QString());
    QCOMPARE(session.sourceUnit("A", "y"), QString());
    QCOMPARE(session.sourceUnit("B", "x"), QString());
    QCOMPARE(session.sourceMeasurement("A", "y"), QVector<double>({3.0}));
    QCOMPARE(session.sourceMeasurement("B", "x"), QVector<double>({5.0}));
}

void ImporterTest::unknownHeaderLinesIgnored()
{
    const QString path = writeTemp("$FLYS,1\n"
                                   "\n"
                                   "$FUTURE,whatever,1,2\n"
                                   "some free text\n"
                                   "$COL,A,time,x\n"
                                   "$DATA\n"
                                   "$A,1,2\n");
    WarningCapture warnings;
    DataImporter importer;
    SessionData session;
    QVERIFY2(importer.readFile(path, session), qPrintable(importer.getLastError()));
    QVERIFY(session.attributeKeys().isEmpty());
    QCOMPARE(session.sourceMeasurement("A", "x"), QVector<double>({2.0}));
    QCOMPARE(warnings.count(), 0);
}

// ─────────────────────────────── data rows

void ImporterTest::malformedRowsAreSkipped()
{
    // IMU columns: time, wy, ax, wz, wx, temperature; first good row 3,-125,1,0,62.5,40
    Fs2FileBuilder file = Fixtures::sensorFile();
    file.rawDataLine("$IMU,4,1,2")                   // too few fields
        .rawDataLine("$IMU,5,,1,0,62.5,40")          // empty field
        .rawDataLine("$IMU,6,x,1,0,62.5,40")         // not a number
        .rawDataLine("$IMU,6,1,1,0,62.5,40,99")      // too many fields
        .rawDataLine("$IMU,2024-13-45T99:00:00Z,1,1,0,62.5,40")  // not a date
        .rawDataLine("$NOPE,1,2")                    // no $COL for this sensor
        .rawDataLine("no tag at all")
        .rawDataLine("")                             // blank: not a row, not counted
        .row("IMU", "8,-250,2,0.5,125,41")
        .rawDataLine("$IMU,9,-12");                  // truncated by power loss
    const QString path = writeTemp(file.toBytes());

    WarningCapture warnings;
    DataImporter importer;
    SessionData session;
    QVERIFY2(importer.readFile(path, session), qPrintable(importer.getLastError()));

    QCOMPARE(warnings.count(), 1);
    QVERIFY2(warnings.messages().at(0).endsWith(QStringLiteral("skipped 8 malformed data row(s)")),
             qPrintable(warnings.messages().at(0)));
    QVERIFY(warnings.messages().at(0).contains(QStringLiteral("file.csv")));

    // All or nothing per row: two samples in every IMU column.
    QCOMPARE(session.sourceMeasurement("IMU", "time"), QVector<double>({3.0, 8.0}));
    QCOMPARE(session.sourceMeasurement("IMU", "wy"), QVector<double>({-125.0, -250.0}));
    QCOMPARE(session.sourceMeasurement("IMU", "ax"), QVector<double>({1.0, 2.0}));
    QCOMPARE(session.sourceMeasurement("IMU", "wz"), QVector<double>({0.0, 0.5}));
    QCOMPARE(session.sourceMeasurement("IMU", "wx"), QVector<double>({62.5, 125.0}));
    QCOMPARE(session.sourceMeasurement("IMU", "temperature"), QVector<double>({40.0, 41.0}));
    QCOMPARE(session.sourceMeasurement("MAG", "x"), QVector<double>({1.0}));
    QCOMPARE(session.sensorKeys(), QStringList({"IMU", "MAG"}));
}

void ImporterTest::cleanFileIsSilent()
{
    const QString sensorPath = writeTemp(Fixtures::sensorFile().toBytes());
    const QString trackPath = writeTemp(Fixtures::trackFile().toBytes());

    WarningCapture warnings;
    DataImporter importer;
    SessionData sensor;
    SessionData track;
    QVERIFY(importer.importFile(sensorPath, sensor));
    QVERIFY(importer.importFile(trackPath, track));
    QCOMPARE(warnings.count(), 0);
}

void ImporterTest::isoTimestamps()
{
    Fs2FileBuilder file = Fixtures::trackFile();
    file.row("GNSS", "2024-01-01T12:00:00.123Z,45.5003,-73.2503,3997,11,-21,6,1.5,2.5,0.25,13");

    DataImporter importer;
    SessionData session;
    QVERIFY(importer.readFile(writeTemp(file.toBytes()), session));

    const QVector<double> time = session.sourceMeasurement("GNSS", "time");
    QCOMPARE(time.size(), 4);
    QVERIFY(qAbs(time.at(0) - 1704110400.0) <= 1e-6);
    QVERIFY(qAbs(time.at(1) - 1704110400.2) <= 1e-6);
    QVERIFY(qAbs(time.at(2) - 1704110400.4) <= 1e-6);
    QVERIFY(qAbs(time.at(3) - 1704110400.123) <= 1e-6);
}

void ImporterTest::storesAsRecorded()
{
    DataImporter importer;
    SessionData session;
    QVERIFY(importer.readFile(writeTemp(Fixtures::sensorFile().toBytes()), session));

    // Samples as parsed and unit text verbatim: nothing is converted at import.
    QCOMPARE(session.sourceMeasurement("IMU", "ax"), QVector<double>({1.0}));
    QCOMPARE(session.sourceUnit("IMU", "ax"), QStringLiteral("g"));
    QCOMPARE(session.sourceMeasurement("MAG", "x"), QVector<double>({1.0}));
    QCOMPARE(session.sourceMeasurement("MAG", "z"), QVector<double>({-0.5}));
    QCOMPARE(session.sourceUnit("MAG", "x"), QStringLiteral("gauss"));
    QCOMPARE(session.sourceMeasurement("IMU", "temperature"), QVector<double>({40.0}));
    QCOMPARE(session.sourceUnit("IMU", "temperature"), QStringLiteral("deg C"));
    QCOMPARE(session.sourceMeasurement("IMU", "wx"), QVector<double>({62.5}));
    QCOMPARE(session.sourceUnit("IMU", "wx"), QStringLiteral("deg/s"));

    // Header attributes verbatim, as strings, in nobody's order but the map's.
    QCOMPARE(session.attributeKeys(), QStringList({"DEVICE_ID", "FIRMWARE_VER", "SESSION_ID"}));
    QCOMPARE(session.getAttribute("FIRMWARE_VER").toString(), QStringLiteral("v2023.09.22"));

    // Importing computes nothing.
    QCOMPARE(session.calculationEngine().totalRunCount(), 0);
}

// The exporter writes non-finite source samples as "nan", "inf",
// "-inf"; the importer reads them back in place instead of skipping the row.
void ImporterTest::nonFiniteTokens()
{
    // Columns: time, wy, ax, wz, wx, temperature
    const QByteArray bytes = Fixtures::sensorFile()
        .row("IMU", "4,nan,1,0,62.5,40")
        .row("IMU", "5,-125,inf,0,62.5,40")
        .row("IMU", "6,-125,1,-inf,62.5,40")
        .toBytes();
    const QString path = writeTemp(bytes);

    WarningCapture warnings;
    DataImporter importer;
    SessionData session;
    QVERIFY2(importer.importFile(path, session), qPrintable(importer.getLastError()));

    for (const QString &column : session.measurementKeys("IMU"))
        QCOMPARE(session.sourceMeasurement("IMU", column).size(), 4);

    QVERIFY(std::isnan(session.sourceMeasurement("IMU", "wy")[1]));
    QVERIFY(session.sourceMeasurement("IMU", "ax")[2] == std::numeric_limits<double>::infinity());
    QVERIFY(session.sourceMeasurement("IMU", "wz")[3] == -std::numeric_limits<double>::infinity());
    QCOMPARE(session.sourceMeasurement("IMU", "time"), QVector<double>({3.0, 4.0, 5.0, 6.0}));

    for (const QString &message : warnings.messages())
        QVERIFY2(!message.contains(QStringLiteral("skipped")), qPrintable(message));
}

// ─────────────────────────────── formats

void ImporterTest::fs1StillImports()
{
    Fs1FileBuilder builder;
    builder.columns({"time", "lat", "lon", "hMSL", "velN", "velE", "velD",
                     "hAcc", "vAcc", "sAcc", "heading", "cAcc", "gpsFix", "numSV"})
           .units({"", "(deg)", "(deg)", "(m)", "(m/s)", "(m/s)", "(m/s)",
                   "(m)", "(m)", "(m/s)", "(deg)", "(deg)", "", ""})
           .row("2024-01-01T12:00:00.00Z,45.5,-73.25,4000.5,10,-20,5,1.5,2.5,0.25,296.5,1.25,3,12")
           .row("2024-01-01T12:00:00.20Z,45.5001,-73.2501,3999.5,10.5,-20.5,5.5,1.5,2.5,0.25,297.5,1.25,3,13");
    const QString path = writeTemp(builder.toBytes());

    DataImporter importer;
    SessionData session;
    QVERIFY2(importer.readFile(path, session), qPrintable(importer.getLastError()));

    QCOMPARE(session.sensorKeys(), QStringList({"GNSS"}));
    QCOMPARE(session.measurementKeys("GNSS").size(), 14);
    QCOMPARE(session.sourceMeasurement("GNSS", "lat"), QVector<double>({45.5, 45.5001}));
    QCOMPARE(session.sourceMeasurement("GNSS", "lon"), QVector<double>({-73.25, -73.2501}));
    QCOMPARE(session.sourceMeasurement("GNSS", "hMSL"), QVector<double>({4000.5, 3999.5}));
    QCOMPARE(session.sourceMeasurement("GNSS", "velD"), QVector<double>({5.0, 5.5}));
    QCOMPARE(session.sourceMeasurement("GNSS", "heading"), QVector<double>({296.5, 297.5}));
    QCOMPARE(session.sourceMeasurement("GNSS", "numSV"), QVector<double>({12.0, 13.0}));

    const QVector<double> time = session.sourceMeasurement("GNSS", "time");
    QCOMPARE(time.size(), 2);
    QVERIFY(qAbs(time.at(0) - 1704110400.0) <= 1e-6);
    QVERIFY(qAbs(time.at(1) - 1704110400.2) <= 1e-6);

    // Unit text is stored verbatim; the conversion layer normalizes it at read time.
    QCOMPARE(session.sourceUnit("GNSS", "hMSL"), QStringLiteral("(m)"));
    QCOMPARE(session.sourceUnit("GNSS", "velN"), QStringLiteral("(m/s)"));
    QCOMPARE(session.sourceUnit("GNSS", "time"), QString());

    // FS1 files declare no attributes.
    QVERIFY(session.attributeKeys().isEmpty());
}

void ImporterTest::fs1WithoutUnitLine()
{
    DataImporter importer;
    SessionData session;
    QVERIFY2(importer.readFile(writeTemp("time,lat,lon,hMSL\n"), session), qPrintable(importer.getLastError()));

    QCOMPARE(session.measurementKeys("GNSS"), QStringList({"hMSL", "lat", "lon", "time"}));
    QVERIFY(session.sourceMeasurement("GNSS", "hMSL").isEmpty());
    QCOMPARE(session.sourceUnit("GNSS", "hMSL"), QString());
}

void ImporterTest::customColumnsAndSensors()
{
    Fs2FileBuilder file;
    file.var("SESSION_ID", "custom")
        .var("DEVICE_ID", "test-device")
        .sensor("FOO", {"time", "bar#1", "baz"}, {"s", "furlongs", ""})
        .row("FOO", "3,7,-1.5e3");

    WarningCapture warnings;
    DataImporter importer;
    SessionData session;
    QVERIFY2(importer.importFile(writeTemp(file.toBytes()), session), qPrintable(importer.getLastError()));
    QCOMPARE(warnings.count(), 0);

    QCOMPARE(session.sensorKeys(), QStringList({"FOO"}));
    QCOMPARE(session.measurementKeys("FOO"), QStringList({"bar#1", "baz", "time"}));
    QCOMPARE(session.sourceMeasurement("FOO", "time"), QVector<double>({3.0}));
    QCOMPARE(session.sourceMeasurement("FOO", "bar#1"), QVector<double>({7.0}));
    QCOMPARE(session.sourceMeasurement("FOO", "baz"), QVector<double>({-1500.0}));
    QCOMPARE(session.sourceUnit("FOO", "bar#1"), QStringLiteral("furlongs"));
    QCOMPARE(session.sourceUnit("FOO", "baz"), QString());
}

void ImporterTest::crlfLineEndings()
{
    DataImporter importer;

    SessionData lf;
    QVERIFY(importer.readFile(writeTemp(Fixtures::sensorFile().toBytes()), lf));

    SessionData crlf;
    Fs2FileBuilder file = Fixtures::sensorFile();
    file.lineEnding("\r\n");
    QVERIFY(file.toBytes().contains("$DATA\r\n"));
    QVERIFY2(importer.readFile(writeTemp(file.toBytes()), crlf), qPrintable(importer.getLastError()));

    QVERIFY(crlf.sourceData() == lf.sourceData());
    QCOMPARE(crlf.attributeKeys(), lf.attributeKeys());
    QCOMPARE(crlf.getAttribute("DEVICE_ID").toString(), QStringLiteral("test-device"));
    QCOMPARE(crlf.sourceMeasurement("IMU", "temperature"), QVector<double>({40.0}));
    QCOMPARE(crlf.sourceMeasurement("IMU", "wx"), QVector<double>({62.5}));
}

void ImporterTest::existingErrorsUnchanged()
{
    DataImporter importer;
    SessionData session;

    QVERIFY(!importer.importFile(TestEnvironment::instance().newTempDir() + "/missing.csv", session));
    QCOMPARE(importer.getLastError(), QStringLiteral("Couldn't read file"));

    QVERIFY(!importer.importFile(writeTemp(""), session));
    QCOMPARE(importer.getLastError(), QStringLiteral("Empty file"));

    QVERIFY(!importer.importFile(writeTemp("hello\nworld\n"), session));
    QCOMPARE(importer.getLastError(), QStringLiteral("Unknown file format"));

    QVERIFY(nothingPublished(session));
}

// ─────────────────────────────── parse result versus session creation

void ImporterTest::parseFileCarriesNoDefaults()
{
    const QString path = writeTemp(Fixtures::sensorFile().toBytes());

    DataImporter importer;
    ParsedFile file;
    QVERIFY2(importer.parseFile(path, file), qPrintable(importer.getLastError()));

    // Exactly what the file declares: no _DESCRIPTION, no _IMPORT_TIME, ...
    QCOMPARE(file.data.attributeKeys(), QStringList({"DEVICE_ID", "FIRMWARE_VER", "SESSION_ID"}));
    QCOMPARE(file.data.sensorKeys(), QStringList({"IMU", "MAG"}));
    QCOMPARE(file.filePath, path);
    QCOMPARE(file.sessionId, QStringLiteral("test-session"));
    QVERIFY(file.sessionIdRecorded);
    QVERIFY(file.applyCreationDefaults);
}

void ImporterTest::parseFileSynthesizesMatchId()
{
    // MD5 sums computed once with md5sum on these exact bytes.
    const QByteArray fs1 =
        "time,lat,lon,hMSL,velN,velE,velD,hAcc,vAcc,sAcc,heading,cAcc,gpsFix,numSV\n"
        ",(deg),(deg),(m),(m/s),(m/s),(m/s),(m),(m),(m/s),(deg),(deg),,\n"
        "2024-01-01T12:00:00.00Z,45.5,-73.25,4000.5,10,-20,5,1.5,2.5,0.25,296.5,1.25,3,12\n";
    const QByteArray fs2 =
        "$FLYS,1\n"
        "$VAR,FIRMWARE_VER,v2023.09.22\n"
        "$COL,GNSS,time,lat,lon,hMSL\n"
        "$UNIT,GNSS,,deg,deg,m\n"
        "$DATA\n"
        "$GNSS,2024-01-01T12:00:00.000Z,45.5,-73.25,4000\n";

    {
        DataImporter importer;
        ParsedFile file;
        QVERIFY2(importer.parseFile(writeTemp(fs1, QStringLiteral("a.csv")), file), qPrintable(importer.getLastError()));
        QVERIFY(!file.sessionIdRecorded);
        QCOMPARE(file.sessionId, QStringLiteral("277eace764c3329f1f7fab1dc85c64c6"));
        QVERIFY(file.data.attributeKeys().isEmpty());
        QVERIFY(!file.data.hasAttribute("SESSION_ID"));
        QVERIFY(!file.data.hasAttribute("DEVICE_ID"));

        // The id follows the bytes, not the path
        ParsedFile again;
        QVERIFY(importer.parseFile(writeTemp(fs1, QStringLiteral("elsewhere.csv")), again));
        QCOMPARE(again.sessionId, QStringLiteral("277eace764c3329f1f7fab1dc85c64c6"));
    }
    {
        DataImporter importer;
        ParsedFile file;
        QVERIFY2(importer.parseFile(writeTemp(fs2), file), qPrintable(importer.getLastError()));
        QVERIFY(!file.sessionIdRecorded);
        QCOMPARE(file.sessionId, QStringLiteral("689427db5cd64fdd5bc096ed70e8a07a"));
        QCOMPARE(file.data.attributeKeys(), QStringList({"FIRMWARE_VER"}));
        QVERIFY(!file.data.hasAttribute("SESSION_ID"));
        QVERIFY(!file.data.hasAttribute("DEVICE_ID"));

        // Creation is what stores the synthesized id
        SessionData created = file.data;
        DataImporter::applyCreationDefaults(file, created);
        QCOMPARE(created.storedAttribute("SESSION_ID").toString(),
                 QStringLiteral("689427db5cd64fdd5bc096ed70e8a07a"));
    }
}

void ImporterTest::parseFailureLeavesResultEmpty()
{
    DataImporter importer;
    ParsedFile file;
    QVERIFY(importer.parseFile(writeTemp(Fixtures::sensorFile().toBytes()), file));

    // The same object, reused for a file that is rejected
    QVERIFY(!importer.parseFile(writeTemp(Fixtures::sensorFile().var("SCHEMA_VER", "3").toBytes()), file));
    QCOMPARE(importer.getLastError(), QStringLiteral("Unsupported SCHEMA_VER '3' (supported: 1, 2)"));
    QVERIFY(nothingPublished(file.data));
    QVERIFY(file.filePath.isEmpty());
    QVERIFY(file.sessionId.isEmpty());
    QVERIFY(!file.sessionIdRecorded);
}

void ImporterTest::creationDefaultsOnlyFillAbsent()
{
    // A Viewer-saved file imported as a new session keeps its own values.
    const QString path = writeTemp(Fixtures::sensorFile()
                                       .var("_DESCRIPTION", "kept")
                                       .var("_JUMPER_MASS", "80")
                                       .toBytes());
    DataImporter importer;
    ParsedFile file;
    QVERIFY2(importer.parseFile(path, file), qPrintable(importer.getLastError()));

    SessionData session = file.data;
    DataImporter::applyCreationDefaults(file, session);

    QCOMPARE(session.storedAttribute("_DESCRIPTION").toString(), QStringLiteral("kept"));
    QCOMPARE(session.storedAttribute("_JUMPER_MASS").toString(), QStringLiteral("80"));
    QCOMPARE(session.storedAttribute("_PLANFORM_AREA").toDouble(), 1.0);
    QVERIFY(session.hasStoredAttribute("_WIND_N"));
    QCOMPARE(session.storedAttribute("_WIND_N").toDouble(), 0.0);
    QCOMPARE(session.storedAttribute("_WIND_E").toDouble(), 0.0);
    QVERIFY(session.storedAttribute("_IMPORT_TIME").toDouble() > 0.0);
    QCOMPARE(session.storedAttribute("DEVICE_ID").toString(), QStringLiteral("test-device"));
    QCOMPARE(session.storedAttribute("SESSION_ID").toString(), QStringLiteral("test-session"));
    QVERIFY(!session.hasStoredAttribute("_GROUND_ELEV"));
    QVERIFY(!session.hasStoredAttribute("SCHEMA_VER"));

    // The parse result itself was not touched
    QCOMPARE(file.data.attributeKeys(),
             QStringList({"DEVICE_ID", "FIRMWARE_VER", "SESSION_ID", "_DESCRIPTION", "_JUMPER_MASS"}));
}

void ImporterTest::creationDefaultsDeviceId()
{
    Fs2FileBuilder noDevice;
    noDevice.var("SESSION_ID", "no-device")
            .sensor("GNSS", {"time", "lat", "lon", "hMSL"}, {"", "deg", "deg", "m"})
            .row("GNSS", "2024-01-01T12:00:00.000Z,45.5,-73.25,4000");

    // A device tree: FLYSIGHT.TXT at the root, the file in YY-MM-DD/HH-MM-SS
    {
        const QString root = TestEnvironment::instance().newTempDir(QStringLiteral("device"));
        QVERIFY(QDir(root).mkpath(QStringLiteral("24-01-01/12-00-00")));
        QVERIFY(writeFile(root + QStringLiteral("/FLYSIGHT.TXT"),
                          "; FlySight 2\nFirmware_Ver: v2023.09.22\nDevice_ID: abc123 ; comment\n"));
        const QString path = root + QStringLiteral("/24-01-01/12-00-00/TRACK.CSV");
        QVERIFY(noDevice.write(path));

        DataImporter importer;
        ParsedFile file;
        QVERIFY2(importer.parseFile(path, file), qPrintable(importer.getLastError()));
        QVERIFY(!file.data.hasAttribute("DEVICE_ID"));

        SessionData session = file.data;
        DataImporter::applyCreationDefaults(file, session);
        QCOMPARE(session.storedAttribute("DEVICE_ID").toString(), QStringLiteral("abc123"));
        QCOMPARE(session.storedAttribute("_DESCRIPTION").toString(), QStringLiteral("24-01-01/12-00-00"));
    }

    // A FlySight 1 card names the device differently
    {
        const QString root = TestEnvironment::instance().newTempDir(QStringLiteral("device-fs1"));
        QVERIFY(writeFile(root + QStringLiteral("/FLYSIGHT.TXT"), "Processor serial number: 0042\n"));
        const QString path = root + QStringLiteral("/track.csv");
        QVERIFY(noDevice.write(path));

        DataImporter importer;
        ParsedFile file;
        QVERIFY(importer.parseFile(path, file));
        SessionData session = file.data;
        DataImporter::applyCreationDefaults(file, session);
        QCOMPARE(session.storedAttribute("DEVICE_ID").toString(), QStringLiteral("0042"));
        QCOMPARE(session.storedAttribute("_DESCRIPTION").toString(), QStringLiteral("track.csv"));
    }

    // No FLYSIGHT.TXT above the file: the placeholder, silently
    {
        const QString path = writeTemp(noDevice.toBytes());
        DataImporter importer;
        ParsedFile file;
        QVERIFY(importer.parseFile(path, file));

        WarningCapture warnings;
        SessionData session = file.data;
        DataImporter::applyCreationDefaults(file, session);
        QCOMPARE(session.storedAttribute("DEVICE_ID").toString(), QStringLiteral("n/a"));
        QCOMPARE(warnings.count(), 0);
    }

    // A session built in memory has no path: no lookup and no placeholder
    {
        SessionData built;
        built.setAttribute("SESSION_ID", QStringLiteral("built"));
        ParsedFile file = ParsedFile::fromSession(built);
        QVERIFY(file.filePath.isEmpty());
        QVERIFY(!file.applyCreationDefaults);
        QCOMPARE(file.sessionId, QStringLiteral("built"));
        QVERIFY(file.sessionIdRecorded);

        SessionData session = file.data;
        DataImporter::applyCreationDefaults(file, session);
        QVERIFY(!session.hasStoredAttribute("DEVICE_ID"));
    }
}

void ImporterTest::fixedGroundElevationOnlyInFixedMode()
{
    PreferencesManager &prefs = PreferencesManager::instance();
    const QString path = writeTemp(Fixtures::trackFile().toBytes());

    prefs.setValue(PreferenceKeys::ImportGroundReferenceMode, QStringLiteral("Fixed"));
    prefs.setValue(PreferenceKeys::ImportFixedElevation, 123.5);
    {
        DataImporter importer;
        SessionData session;
        QVERIFY(importer.importFile(path, session));
        QVERIFY(session.hasStoredAttribute("_GROUND_ELEV"));
        QCOMPARE(session.storedAttribute("_GROUND_ELEV").toDouble(), 123.5);
    }

    prefs.setValue(PreferenceKeys::ImportGroundReferenceMode, QStringLiteral("Automatic"));
    {
        DataImporter importer;
        SessionData session;
        QVERIFY(importer.importFile(path, session));
        QVERIFY(!session.hasStoredAttribute("_GROUND_ELEV"));
    }
}

void ImporterTest::peekHeaderAttributeReadsNoData()
{
    // The data section is binary junk no parser would accept: the peek never
    // gets there.
    QByteArray bytes =
        "$FLYS,1\r\n"
        "$VAR,SESSION_ID_X,decoy\r\n"
        "$VAR,SESSION_ID,abc,def\r\n"
        "$VAR,EMPTY\r\n"
        "$COL,GNSS,time,lat\r\n"
        "$DATA\r\n";
    bytes += QByteArray::fromHex("00fffe");
    bytes += "$VAR,AFTER_DATA,no\n";
    bytes += QByteArray(4096, '\xff');
    const QString path = writeTemp(bytes);

    QCOMPARE(DataImporter::peekHeaderAttribute(path, QStringLiteral("SESSION_ID")).value_or(QStringLiteral("<none>")),
             QStringLiteral("abc,def"));
    QCOMPARE(DataImporter::peekHeaderAttribute(path, QStringLiteral("EMPTY")).value_or(QStringLiteral("<none>")),
             QString());
    QVERIFY(!DataImporter::peekHeaderAttribute(path, QStringLiteral("AFTER_DATA")).has_value());
    QVERIFY(!DataImporter::peekHeaderAttribute(path, QStringLiteral("MISSING")).has_value());
    QVERIFY(!DataImporter::peekHeaderAttribute(path + QStringLiteral(".nope"), QStringLiteral("SESSION_ID")).has_value());
}

FLYSIGHT_TEST_MAIN(ImporterTest)

#include "tst_importer.moc"
