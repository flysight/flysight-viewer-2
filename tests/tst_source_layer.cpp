// Session-level acceptance suite for the source layer and the conversion
// layer: real SessionData, the real importer, exporter, logbook and model, and
// the process-wide registry with every built-in.
//
// Comparison rule. Every expected value is a literal; the only computed
// comparison is the engine's fresh-evaluation oracle (verifyAgainstFresh).
// 62.5 * 1.14688 evaluates to 71.679999999999993, which is NOT the double
// nearest 71.68, so corrected gyro values are always compared with the explicit
// absolute tolerance of 1e-9 (isNear), never with ==. Values whose conversion
// multiplies exactly representable inputs (1 g, 1 gauss, identities, zero) are
// compared with ==.
//
// A test that asserts effective values needs registerBuiltIns(): without it no
// conversion family exists and effective == source.

#include <QDir>
#include <QtTest>

#include "dataexporter.h"
#include "dataimporter.h"
#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "fixturebuilder.h"
#include "logbookmanager.h"
#include "sessiondata.h"
#include "sessionmodel.h"
#include "testenvironment.h"
#include "testmain.h"

using namespace FlySight;
using namespace FlySightTest;

namespace {

bool isNear(double a, double b)
{
    return qAbs(a - b) <= 1e-9;
}

DependencyKey meas(const char *sensor, const char *name)
{
    return DependencyKey::measurement(QString::fromLatin1(sensor), QString::fromLatin1(name));
}

int g_warningCount = 0;

void countingHandler(QtMsgType type, const QMessageLogContext &, const QString &)
{
    if (type == QtWarningMsg || type == QtCriticalMsg)
        ++g_warningCount;
}

// Counts warnings for as long as it lives. Installed after registerBuiltIns().
class WarningCounter {
public:
    WarningCounter()
    {
        g_warningCount = 0;
        m_previous = qInstallMessageHandler(countingHandler);
    }
    ~WarningCounter() { qInstallMessageHandler(m_previous); }
    int count() const { return g_warningCount; }

private:
    QtMessageHandler m_previous = nullptr;
};

QString writeTemp(const Fs2FileBuilder &file, const QString &stem)
{
    const QString path = TestEnvironment::instance().newTempDir(stem)
                         + QLatin1Char('/') + stem + QStringLiteral(".csv");
    if (!file.write(path))
        qFatal("could not write %s", qPrintable(path));
    return path;
}

bool importInto(SessionData &session, const Fs2FileBuilder &file, const QString &stem)
{
    DataImporter importer;
    return importer.importFile(writeTemp(file, stem), session);
}

// The single effective sample of a one-row column, NaN otherwise.
double first(const SessionData &session, const char *sensor, const char *name)
{
    const QVector<double> values = session.getMeasurement(sensor, name);
    return values.size() == 1 ? values.at(0) : qQNaN();
}

QStringList sessionCsvFiles()
{
    return QDir(TestEnvironment::instance().sessionsDir())
        .entryList({QStringLiteral("*.csv")}, QDir::Files, QDir::Name);
}

const QList<DependencyKey> gyroAndFriends = {
    meas("IMU", "wx"), meas("IMU", "wy"), meas("IMU", "wz"), meas("IMU", "wTotal"), meas("IMU", "ax")};

} // namespace

class SourceLayerTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void unmarkedFileIsCorrected();
    void schema2FileIsLiteral();
    void unitNormalization();
    void releasedLogbookFormatLoads();
    void derivedWTotalUsesCorrectedGyro();
    void interpolatedGyroAttribute();
    void fileSuppliedWTotalWins();
    void schemaAttributeFlipsGyro();

    void enumerationIgnoresComputed();
    void sourceAccessNeverComputes();
    void sourceAccessNeverCreatesEngine();
    void setUnitNeedsSourceData();
    void lazyConversion();
    void identitySharesBuffer();

    void exporterWritesSource();
    void mergeCopiesSource();
    void registryStartsWithConversionLayer();
};

void SourceLayerTest::initTestCase()
{
    TestEnvironment::instance().registerBuiltIns();
}

void SourceLayerTest::init()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();
    env.resetPreferencesToDefaults();
}

// Acceptance 1: an unmarked file with wx=62.5, wy=-125, wz=0 reads as
// 71.68, -143.36, 0 deg/s; source access returns what was recorded;
// SCHEMA_VER stays absent.
void SourceLayerTest::unmarkedFileIsCorrected()
{
    SessionData session;
    QVERIFY(importInto(session, Fixtures::sensorFile(), QStringLiteral("unmarked")));

    QVERIFY(isNear(first(session, "IMU", "wx"), 71.68));
    QVERIFY(isNear(first(session, "IMU", "wy"), -143.36));
    QVERIFY(first(session, "IMU", "wz") == 0.0);

    QCOMPARE(session.sourceMeasurement("IMU", "wx"), QVector<double>({62.5}));
    QCOMPARE(session.sourceMeasurement("IMU", "wy"), QVector<double>({-125.0}));
    QCOMPARE(session.sourceMeasurement("IMU", "wz"), QVector<double>({0.0}));

    QCOMPARE(session.sourceUnit("IMU", "wx"), QStringLiteral("deg/s"));
    QCOMPARE(session.effectiveUnit("IMU", "wx"), QStringLiteral("deg/s"));
    QCOMPARE(session.effectiveUnit("IMU", "wy"), QStringLiteral("deg/s"));

    // Absence is interpreted inside the conversion layer and never written back.
    QVERIFY(!session.hasAttribute("SCHEMA_VER"));
    QVERIFY(!session.attributeKeys().contains(QStringLiteral("SCHEMA_VER")));
    QVERIFY(!session.getAttribute("SCHEMA_VER").isValid());
}

// Acceptance 2: the same values in a file declaring SCHEMA_VER 2 are unchanged.
void SourceLayerTest::schema2FileIsLiteral()
{
    SessionData session;
    QVERIFY(importInto(session, Fixtures::sensorFile().var("SCHEMA_VER", "2"), QStringLiteral("schema2")));

    QCOMPARE(session.getMeasurement("IMU", "wx"), QVector<double>({62.5}));
    QCOMPARE(session.getMeasurement("IMU", "wy"), QVector<double>({-125.0}));
    QCOMPARE(session.getMeasurement("IMU", "wz"), QVector<double>({0.0}));
    QCOMPARE(session.effectiveUnit("IMU", "wx"), QStringLiteral("deg/s"));

    QCOMPARE(session.getAttribute("SCHEMA_VER").toString(), QStringLiteral("2"));
    QVERIFY(session.hasAttribute("SCHEMA_VER"));

    // Everything else is converted exactly as in an unmarked file.
    QVERIFY(first(session, "IMU", "ax") == 9.80665);
}

// Acceptance 4: 1 g reads as 9.80665 m/s^2, 1 gauss as 0.0001 T, the source
// keeps 1 and g / gauss; a custom column with unknown unit text passes through.
void SourceLayerTest::unitNormalization()
{
    Fs2FileBuilder file = Fixtures::sensorFile();
    file.sensor("FOO", {"time", "bar"}, {"s", "furlongs"})
        .row("FOO", "3,7");
    const QString path = writeTemp(file, QStringLiteral("units"));

    WarningCounter warnings;

    DataImporter importer;
    SessionData session;
    QVERIFY(importer.importFile(path, session));

    QVERIFY(first(session, "IMU", "ax") == 9.80665);
    QCOMPARE(session.effectiveUnit("IMU", "ax"), QStringLiteral("m/s^2"));
    QCOMPARE(session.sourceMeasurement("IMU", "ax"), QVector<double>({1.0}));
    QCOMPARE(session.sourceUnit("IMU", "ax"), QStringLiteral("g"));

    QVERIFY(first(session, "MAG", "x") == 0.0001);
    QVERIFY(first(session, "MAG", "z") == -0.00005);
    QCOMPARE(session.effectiveUnit("MAG", "x"), QStringLiteral("T"));
    QCOMPARE(session.effectiveUnit("MAG", "z"), QStringLiteral("T"));
    QCOMPARE(session.sourceMeasurement("MAG", "x"), QVector<double>({1.0}));
    QCOMPARE(session.sourceMeasurement("MAG", "z"), QVector<double>({-0.5}));
    QCOMPARE(session.sourceUnit("MAG", "x"), QStringLiteral("gauss"));

    QVERIFY(first(session, "IMU", "temperature") == 40.0);
    QCOMPARE(session.effectiveUnit("IMU", "temperature"), QStringLiteral("degC"));
    QCOMPARE(session.sourceUnit("IMU", "temperature"), QStringLiteral("deg C"));

    QVERIFY(first(session, "FOO", "bar") == 7.0);
    QCOMPARE(session.effectiveUnit("FOO", "bar"), QStringLiteral("furlongs"));
    QCOMPARE(session.sourceUnit("FOO", "bar"), QStringLiteral("furlongs"));

    // Silence for the normal case, unknown unit text included.
    QCOMPARE(warnings.count(), 0);
}

// Acceptance 6 (load part): a released-format logbook file - normalized values
// and labels, no SCHEMA_VER - loads unchanged and its gyro is corrected once.
void SourceLayerTest::releasedLogbookFormatLoads()
{
    TestEnvironment &env = TestEnvironment::instance();
    LogbookManager &logbook = LogbookManager::instance();
    logbook.initialize();

    // Let the logbook create the entry, then put a file on disk as a released
    // Viewer version wrote it.
    SessionData placeholder;
    placeholder.setAttribute("SESSION_ID", QStringLiteral("rel"));
    placeholder.setAttribute("DEVICE_ID", QStringLiteral("test-device"));
    placeholder.setSourceMeasurement("IMU", "time", {3.0}, "s");
    QVERIFY(logbook.saveSession(placeholder));

    const QStringList files = sessionCsvFiles();
    QCOMPARE(files.size(), 1);
    const QString filePath = env.sessionsDir() + QLatin1Char('/') + files.first();

    const QByteArray released =
        "$FLYS,1\n"
        "$VAR,DEVICE_ID,test-device\n"
        "$VAR,SESSION_ID,rel\n"
        "$VAR,FIRMWARE_VER,v2023.09.22\n"
        "$VAR,_DESCRIPTION,Released, with a comma\n"
        "$COL,MAG,time,x\n"
        "$UNIT,MAG,s,T\n"
        "$COL,IMU,time,wx,wy,wz,ax,temperature\n"
        "$UNIT,IMU,s,deg/s,deg/s,deg/s,m/s^2,degC\n"
        "$DATA\n"
        "$MAG,3,0.0001\n"
        "$IMU,3,62.5,-125,0,9.80665,40\n";
    QVERIFY(writeFile(filePath, released));

    logbook.flushIndex();
    env.reopenLogbook();
    logbook.initialize();

    const std::optional<SessionData> loaded = logbook.loadSession(QStringLiteral("rel"));
    QVERIFY(loaded.has_value());

    // Already-internal labels: the identity, on the very same buffer.
    {
        const QVector<double> source = loaded->sourceMeasurement("IMU", "ax");
        const QVector<double> effective = loaded->getMeasurement("IMU", "ax");
        QCOMPARE(effective.size(), 1);
        QVERIFY(effective.at(0) == 9.80665);
        QVERIFY(source.constData() == effective.constData());
        QCOMPARE(loaded->effectiveUnit("IMU", "ax"), QStringLiteral("m/s^2"));
        QCOMPARE(loaded->sourceUnit("IMU", "ax"), QStringLiteral("m/s^2"));
    }
    QVERIFY(first(*loaded, "MAG", "x") == 0.0001);
    QCOMPARE(loaded->effectiveUnit("MAG", "x"), QStringLiteral("T"));
    QCOMPARE(loaded->sourceUnit("MAG", "x"), QStringLiteral("T"));
    QVERIFY(first(*loaded, "IMU", "temperature") == 40.0);
    QCOMPARE(loaded->effectiveUnit("IMU", "temperature"), QStringLiteral("degC"));

    // The absent schema means legacy gyro: corrected exactly once (a second
    // correction would give 82.2...).
    QVERIFY(isNear(first(*loaded, "IMU", "wx"), 71.68));
    QVERIFY(isNear(first(*loaded, "IMU", "wy"), -143.36));
    QVERIFY(first(*loaded, "IMU", "wz") == 0.0);
    QCOMPARE(loaded->calculationEngine().runCountForInstance("builtin.conversion.default#IMU/wx"), 1);
    QCOMPARE(loaded->calculationEngine().runCount("builtin.conversion.schema"), 0);
    QCOMPARE(loaded->sourceMeasurement("IMU", "wx"), QVector<double>({62.5}));

    QVERIFY(!loaded->hasAttribute("SCHEMA_VER"));
    QCOMPARE(loaded->getAttribute("_DESCRIPTION").toString(), QStringLiteral("Released, with a comma"));

    // Loading rewrites nothing.
    QCOMPARE(readFileBytes(filePath), released);
}

// Acceptance 16: derived IMU/wTotal reflects the corrected values and follows
// source changes.
void SourceLayerTest::derivedWTotalUsesCorrectedGyro()
{
    SessionData session;
    QVERIFY(importInto(session, Fixtures::sensorFile(), QStringLiteral("wtotal")));

    QVERIFY(isNear(first(session, "IMU", "wTotal"), 160.28135262718));

    const QSet<DependencyKey> invalidated = session.setMeasurement("IMU", "wx", {0.0});
    QVERIFY(invalidated.contains(meas("IMU", "wx")));
    QVERIFY(invalidated.contains(meas("IMU", "wTotal")));
    QVERIFY(!invalidated.contains(meas("IMU", "ax")));

    QVERIFY(isNear(first(session, "IMU", "wTotal"), 143.36));
    QVERIFY(!session.hasMeasurement("IMU", "wTotal"));
}

// Acceptance 16: an interpolated gyro attribute reflects the corrected values
// and follows source changes.
void SourceLayerTest::interpolatedGyroAttribute()
{
    SessionData session;
    // TIME/time, tow, week whose fit is exactly a = 1, b = 1704110400.
    session.setSourceMeasurement("TIME", "time", {10, 20, 30}, "s");
    session.setSourceMeasurement("TIME", "tow", {129610, 129620, 129630}, "s");
    session.setSourceMeasurement("TIME", "week", {2295, 2295, 2295}, "");
    session.setSourceMeasurement("IMU", "time", {10, 20, 30}, "s");
    session.setSourceMeasurement("IMU", "wx", {1, 2, 3}, "deg/s");
    session.setAttribute("_M", 1704110415.0);

    const QString key = SessionData::interpolationKey("_M", "IMU", "_time", "wx");
    QVERIFY(isNear(session.getAttribute(key).toDouble(), 1.72032));     // 1.5 x 1.14688

    const QSet<DependencyKey> invalidated = session.setSourceMeasurement("IMU", "wx", {10, 20, 30}, "deg/s");
    QVERIFY(invalidated.contains(DependencyKey::attribute(key)));
    QVERIFY(isNear(session.getAttribute(key).toDouble(), 17.2032));     // 15 x 1.14688
    QVERIFY(!session.hasAttribute(key));
}

// Acceptance 16: a file-supplied wTotal keeps precedence over the derived one.
void SourceLayerTest::fileSuppliedWTotalWins()
{
    Fs2FileBuilder file;
    file.var("SESSION_ID", "supplied")
        .var("DEVICE_ID", "test-device")
        .sensor("IMU", {"time", "wx", "wy", "wz", "wTotal"}, {"s", "deg/s", "deg/s", "deg/s", "deg/s"})
        .row("IMU", "3,62.5,-125,0,99");

    SessionData session;
    QVERIFY(importInto(session, file, QStringLiteral("supplied")));

    // Not corrected: only wx / wy / wz are in the schema table.
    QCOMPARE(session.getMeasurement("IMU", "wTotal"), QVector<double>({99.0}));
    QCOMPARE(session.calculationEngine().runCount("builtin.imu.wTotal"), 0);
    QVERIFY(session.hasMeasurement("IMU", "wTotal"));
    QVERIFY(isNear(first(session, "IMU", "wx"), 71.68));
}

void SourceLayerTest::schemaAttributeFlipsGyro()
{
    SessionData session;
    QVERIFY(importInto(session, Fixtures::sensorFile(), QStringLiteral("flip")));
    CalculationEngine &engine = session.calculationEngine();

    QVERIFY(isNear(first(session, "IMU", "wx"), 71.68));
    QVERIFY(isNear(first(session, "IMU", "wTotal"), 160.28135262718));
    QVERIFY(first(session, "IMU", "ax") == 9.80665);
    QVERIFY(engine.verifyAgainstFresh(gyroAndFriends).isEmpty());

    // The gyro columns and what is derived from them depend on SCHEMA_VER; nothing else does.
    QSet<DependencyKey> invalidated = session.setAttribute("SCHEMA_VER", QStringLiteral("2"));
    QVERIFY(invalidated.contains(meas("IMU", "wx")));
    QVERIFY(invalidated.contains(meas("IMU", "wTotal")));
    QVERIFY(!invalidated.contains(meas("IMU", "ax")));

    QCOMPARE(session.getMeasurement("IMU", "wx"), QVector<double>({62.5}));
    QVERIFY(isNear(first(session, "IMU", "wTotal"), 139.75424859373686));
    QVERIFY(engine.verifyAgainstFresh(gyroAndFriends).isEmpty());

    invalidated = session.removeAttribute("SCHEMA_VER");
    QVERIFY(invalidated.contains(meas("IMU", "wx")));
    QVERIFY(invalidated.contains(meas("IMU", "wTotal")));
    QVERIFY(!invalidated.contains(meas("IMU", "ax")));

    QVERIFY(isNear(first(session, "IMU", "wx"), 71.68));
    QVERIFY(isNear(first(session, "IMU", "wTotal"), 160.28135262718));
    QVERIFY(engine.verifyAgainstFresh(gyroAndFriends).isEmpty());

    // ax was converted once, however often SCHEMA_VER changed.
    QCOMPARE(engine.runCountForInstance("builtin.conversion.default#IMU/ax"), 1);
}

// Spec section 4: enumeration describes stored source data and stored
// attributes; a calculation output does not appear because it was computed.
void SourceLayerTest::enumerationIgnoresComputed()
{
    SessionData session;
    QVERIFY(importInto(session, Fixtures::sensorFile(), QStringLiteral("enum")));

    const QStringList attributesBefore = session.attributeKeys();

    QVERIFY(!session.getMeasurement("IMU", "wTotal").isEmpty());
    session.getMeasurement("IMU", "aTotal");    // unavailable here (no ay / az): still must not appear
    QVERIFY(!session.getMeasurement("MAG", "total").isEmpty());
    session.getMeasurement("IMU", "_time");     // unavailable here (no TIME sensor), like _START_TIME
    for (const QString &sensor : session.sensorKeys()) {
        for (const QString &name : session.measurementKeys(sensor))
            QVERIFY(!session.getMeasurement(sensor, name).isEmpty());
    }
    session.getAttribute("_START_TIME");
    session.getAttribute("_TIME_FIT_A");

    QCOMPARE(session.sensorKeys(), QStringList({"IMU", "MAG"}));
    QCOMPARE(session.measurementKeys("IMU"),
             QStringList({"ax", "temperature", "time", "wx", "wy", "wz"}));
    QCOMPARE(session.measurementKeys("MAG"), QStringList({"temperature", "time", "x", "y", "z"}));
    QVERIFY(!session.hasMeasurement("IMU", "wTotal"));
    QVERIFY(!session.hasMeasurement("MAG", "total"));
    QVERIFY(!session.hasSensor("Simplified"));
    QVERIFY(!session.hasAttribute("_START_TIME"));
    QCOMPARE(session.attributeKeys(), attributesBefore);

    // The same for calculated attributes and measurements that ARE available.
    SessionData track;
    QVERIFY(importInto(track, Fixtures::trackFile(), QStringLiteral("enum-track")));
    const QStringList trackAttributes = track.attributeKeys();
    const QStringList trackColumns = track.measurementKeys("GNSS");

    QVERIFY(qAbs(track.getAttribute("_START_TIME").toDouble() - 1704110400.0) <= 1e-6);
    QCOMPARE(track.getMeasurement("GNSS", "_time").size(), 3);
    QCOMPARE(track.getMeasurement("GNSS", "velH").size(), 3);

    QVERIFY(!track.hasAttribute("_START_TIME"));
    QCOMPARE(track.attributeKeys(), trackAttributes);
    QCOMPARE(track.sensorKeys(), QStringList({"GNSS"}));
    QCOMPARE(track.measurementKeys("GNSS"), trackColumns);
    QCOMPARE(trackColumns.size(), 11);
    QVERIFY(!track.hasMeasurement("GNSS", "_time"));
    QVERIFY(!track.hasMeasurement("GNSS", "velH"));
}

// Spec section 4: source access never runs a calculation and never falls back
// to a derived value.
void SourceLayerTest::sourceAccessNeverComputes()
{
    SessionData session;
    QVERIFY(importInto(session, Fixtures::sensorFile(), QStringLiteral("nocompute")));
    CalculationEngine &engine = session.calculationEngine();

    QCOMPARE(engine.totalRunCount(), 0);
    QCOMPARE(engine.cachedNodeCount(), 0);

    for (const QString &sensor : session.sensorKeys()) {
        QVERIFY(session.hasSensor(sensor));
        for (const QString &name : session.measurementKeys(sensor)) {
            QVERIFY(session.hasSourceMeasurement(sensor, name));
            QVERIFY(session.hasMeasurement(sensor, name));
            QCOMPARE(session.sourceMeasurement(sensor, name).size(), 1);
            session.sourceUnit(sensor, name);
        }
    }
    QCOMPARE(session.sourceData().value("IMU").value("ax").samples, QVector<double>({1.0}));
    QCOMPARE(session.sourceData().value("IMU").value("ax").unit, QStringLiteral("g"));
    for (const QString &key : session.attributeKeys())
        QVERIFY(session.hasAttribute(key));

    QCOMPARE(engine.totalRunCount(), 0);
    QCOMPARE(engine.cachedNodeCount(), 0);

    // A purely derived name has no source - and asking does not derive it.
    QVERIFY(session.sourceMeasurement("IMU", "wTotal").isEmpty());
    QCOMPARE(session.sourceUnit("IMU", "wTotal"), QString());
    QVERIFY(!session.hasSourceMeasurement("IMU", "wTotal"));
    QVERIFY(!session.sourceData().value("IMU").contains("wTotal"));
    QCOMPARE(engine.totalRunCount(), 0);
    QCOMPARE(engine.cachedNodeCount(), 0);

    // The same with a warm engine: nothing runs, nothing is cached or dropped.
    QVERIFY(isNear(first(session, "IMU", "wTotal"), 160.28135262718));
    const int runs = engine.totalRunCount();
    const int cached = engine.cachedNodeCount();

    QVERIFY(session.sourceMeasurement("IMU", "wTotal").isEmpty());
    QCOMPARE(session.sourceUnit("IMU", "wTotal"), QString());
    QVERIFY(!session.hasSourceMeasurement("IMU", "wTotal"));
    QCOMPARE(session.sourceMeasurement("IMU", "wx"), QVector<double>({62.5}));
    QCOMPARE(session.sourceUnit("IMU", "wx"), QStringLiteral("deg/s"));
    session.sourceData();
    session.sensorKeys();
    session.measurementKeys("IMU");
    session.hasMeasurement("IMU", "wTotal");
    session.attributeKeys();
    session.hasAttribute("_START_TIME");

    QCOMPARE(engine.totalRunCount(), runs);
    QCOMPARE(engine.cachedNodeCount(), cached);
}

// Source accessors and enumeration are called on hundreds of temporary
// sessions during a logbook scan: they must not even create the engine.
void SourceLayerTest::sourceAccessNeverCreatesEngine()
{
    const int enrolledBefore = CalculationRegistry::instance().enrolledEngineCount();

    SessionData session;
    QVERIFY(importInto(session, Fixtures::sensorFile(), QStringLiteral("noengine")));

    session.sourceMeasurement("IMU", "wx");
    session.sourceUnit("IMU", "wx");
    session.hasSourceMeasurement("IMU", "wTotal");
    session.sourceData();
    session.sensorKeys();
    session.hasSensor("IMU");
    session.measurementKeys("IMU");
    session.hasMeasurement("IMU", "wx");
    session.attributeKeys();
    session.hasAttribute("SESSION_ID");
    session.storedAttribute("SESSION_ID");

    // Importing and source access enrolled no engine with the registry.
    QCOMPARE(CalculationRegistry::instance().enrolledEngineCount(), enrolledBefore);

    session.getMeasurement("IMU", "wx");
    QCOMPARE(CalculationRegistry::instance().enrolledEngineCount(), enrolledBefore + 1);
}

void SourceLayerTest::setUnitNeedsSourceData()
{
    SessionData session;
    QVERIFY(importInto(session, Fixtures::sensorFile(), QStringLiteral("setunit")));
    session.getMeasurement("IMU", "wTotal");    // warm engine

    {
        WarningCounter warnings;
        QVERIFY(session.setUnit("IMU", "wTotal", "deg/s").isEmpty());
        QCOMPARE(warnings.count(), 1);
    }
    QCOMPARE(session.sourceUnit("IMU", "wTotal"), QString());
    QVERIFY(!session.sourceData().value("IMU").contains("wTotal"));
    QVERIFY(!session.hasMeasurement("IMU", "wTotal"));

    // With source data the unit is replaced and the effective value follows.
    QVERIFY(session.setUnit("IMU", "ax", "m/s^2").contains(meas("IMU", "ax")));
    QVERIFY(first(session, "IMU", "ax") == 1.0);
    QCOMPARE(session.effectiveUnit("IMU", "ax"), QStringLiteral("m/s^2"));

    // setMeasurement replaces the samples and keeps the recorded unit text.
    session.setMeasurement("MAG", "x", {2.0});
    QCOMPARE(session.sourceUnit("MAG", "x"), QStringLiteral("gauss"));
    QVERIFY(first(session, "MAG", "x") == 0.0002);
}

// Spec 5.3: effective values are computed on first read, not at import.
void SourceLayerTest::lazyConversion()
{
    SessionData session;
    QVERIFY(importInto(session, Fixtures::sensorFile(), QStringLiteral("lazy")));
    CalculationEngine &engine = session.calculationEngine();

    QCOMPARE(engine.totalRunCount(), 0);

    QVERIFY(first(session, "IMU", "ax") == 9.80665);
    QCOMPARE(engine.runCount("builtin.conversion.default"), 1);
    QCOMPARE(engine.totalRunCount(), 1);

    for (int i = 0; i < 10; ++i)
        QVERIFY(first(session, "IMU", "ax") == 9.80665);
    QCOMPARE(engine.runCount("builtin.conversion.default"), 1);
    QCOMPARE(engine.totalRunCount(), 1);
}

// Spec 5.3: a column whose conversion is the identity needs no second copy of
// its samples; a converted column costs one buffer.
void SourceLayerTest::identitySharesBuffer()
{
    SessionData track;
    QVERIFY(importInto(track, Fixtures::trackFile(), QStringLiteral("share-track")));
    SessionData sensor;
    QVERIFY(importInto(sensor, Fixtures::sensorFile(), QStringLiteral("share-sensor")));

    {
        const QVector<double> source = track.sourceMeasurement("GNSS", "hMSL");
        const QVector<double> effective = track.getMeasurement("GNSS", "hMSL");
        QCOMPARE(effective, QVector<double>({4000.0, 3999.0, 3998.0}));
        QVERIFY(source.constData() == effective.constData());
    }
    {
        const QVector<double> source = track.sourceMeasurement("GNSS", "time");
        const QVector<double> effective = track.getMeasurement("GNSS", "time");
        QCOMPARE(effective.size(), 3);
        QVERIFY(source.constData() == effective.constData());

        // GNSS/_time passes the effective time through: still the same buffer.
        const QVector<double> derived = track.getMeasurement("GNSS", "_time");
        QCOMPARE(derived.size(), 3);
        QVERIFY(source.constData() == derived.constData());
    }
    {
        // Label-only change ("deg C" -> "degC")
        const QVector<double> source = sensor.sourceMeasurement("IMU", "temperature");
        const QVector<double> effective = sensor.getMeasurement("IMU", "temperature");
        QCOMPARE(effective, QVector<double>({40.0}));
        QVERIFY(source.constData() == effective.constData());
    }
    {
        const QVector<double> source = sensor.sourceMeasurement("IMU", "ax");
        const QVector<double> effective = sensor.getMeasurement("IMU", "ax");
        QCOMPARE(effective.size(), 1);
        QVERIFY(source.constData() != effective.constData());
    }
    {
        const QVector<double> source = sensor.sourceMeasurement("IMU", "wx");
        const QVector<double> effective = sensor.getMeasurement("IMU", "wx");
        QCOMPARE(effective.size(), 1);
        QVERIFY(source.constData() != effective.constData());
        QCOMPARE(source, QVector<double>({62.5}));
    }
}

// Spec 9.1: the writer reads only the source layer, warm cache or not.
void SourceLayerTest::exporterWritesSource()
{
    SessionData session;
    QVERIFY(importInto(session, Fixtures::sensorFile(), QStringLiteral("export")));

    const QString dir = TestEnvironment::instance().newTempDir(QStringLiteral("exported"));
    const QString coldPath = dir + QStringLiteral("/cold.csv");
    QVERIFY(DataExporter::exportSession(coldPath, session));

    // Warm every effective column and the derived values.
    for (const QString &sensor : session.sensorKeys()) {
        for (const QString &name : session.measurementKeys(sensor))
            QVERIFY(!session.getMeasurement(sensor, name).isEmpty());
    }
    QVERIFY(!session.getMeasurement("IMU", "wTotal").isEmpty());

    const QString warmPath = dir + QStringLiteral("/warm.csv");
    QVERIFY(DataExporter::exportSession(warmPath, session));

    const QByteArray bytes = readFileBytes(warmPath);
    QVERIFY2(bytes.contains("$COL,IMU,time,wx,wy,wz,ax,temperature\n"
                            "$UNIT,IMU,s,deg/s,deg/s,deg/s,g,deg C\n"), bytes.constData());
    QVERIFY(bytes.contains("$COL,MAG,time,x,y,z,temperature\n"
                           "$UNIT,MAG,s,gauss,gauss,gauss,deg C\n"));
    QVERIFY(bytes.contains("$IMU,3,62.5,-125,0,1,40\n"));
    QVERIFY(bytes.contains("$MAG,3,1,0,-0.5,40\n"));

    QVERIFY(!bytes.contains("71.6"));
    QVERIFY(!bytes.contains("9.80665"));
    QVERIFY(!bytes.contains("0.0001"));
    QVERIFY(!bytes.contains("m/s^2"));
    QVERIFY(!bytes.contains("wTotal"));
    QVERIFY(!bytes.contains("SCHEMA_VER"));

    QCOMPARE(bytes, readFileBytes(coldPath));
}

// The merge copies the source layer - samples and unit text together - never
// effective values: those would be stored as if recorded and converted again.
void SourceLayerTest::mergeCopiesSource()
{
    LogbookManager::instance().initialize();

    SessionData track;
    SessionData sensor;
    QVERIFY(importInto(track, Fixtures::trackFile(), QStringLiteral("merge-track")));
    QVERIFY(importInto(sensor, Fixtures::sensorFile(), QStringLiteral("merge-sensor")));
    sensor.getMeasurement("IMU", "wx");     // a warm incoming session changes nothing

    SessionModel model;
    model.mergeSessions({track});
    model.mergeSessions({sensor});          // into the loaded row

    QCOMPARE(model.rowCount(), 1);
    SessionData &merged = model.sessionRef(0);
    QCOMPARE(merged.sensorKeys(), QStringList({"GNSS", "IMU", "MAG"}));

    QCOMPARE(merged.sourceMeasurement("IMU", "ax"), QVector<double>({1.0}));
    QCOMPARE(merged.sourceUnit("IMU", "ax"), QStringLiteral("g"));
    QVERIFY(first(merged, "IMU", "ax") == 9.80665);
    QCOMPARE(merged.sourceMeasurement("IMU", "wx"), QVector<double>({62.5}));
    QCOMPARE(merged.sourceUnit("IMU", "wx"), QStringLiteral("deg/s"));
    QVERIFY(isNear(first(merged, "IMU", "wx"), 71.68));     // not 82.2...
    QCOMPARE(merged.sourceUnit("GNSS", "hMSL"), QStringLiteral("m"));
    QVERIFY(!merged.hasAttribute("SCHEMA_VER"));

    // Leave no deferred save behind for the next test.
    model.flushDirtySessions();
    QCOMPARE(sessionCsvFiles().size(), 1);
    const QByteArray saved = readFileBytes(TestEnvironment::instance().sessionsDir()
                                           + QLatin1Char('/') + sessionCsvFiles().first());
    QVERIFY(saved.contains("$UNIT,IMU,s,deg/s,deg/s,deg/s,g,deg C\n"));
    QVERIFY(saved.contains("$IMU,3,62.5,-125,0,1,40\n"));
    QVERIFY(!saved.contains("SCHEMA_VER"));
}

void SourceLayerTest::registryStartsWithConversionLayer()
{
    const QList<CalculationId> ids = CalculationRegistry::instance().registeredIds();
    QVERIFY(ids.size() > 2);
    QCOMPARE(ids.at(0), QStringLiteral("builtin.conversion.schema"));
    QCOMPARE(ids.at(1), QStringLiteral("builtin.conversion.default"));
    QVERIFY(CalculationRegistry::instance().hasSourceConversions());
}

FLYSIGHT_TEST_MAIN(SourceLayerTest)

#include "tst_source_layer.moc"
