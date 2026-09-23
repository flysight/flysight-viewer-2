// Self-tests of the shared test-support code: isolation, the temporary
// logbook, and the fixture builders.

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QtTest>

#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "fakesessionstate.h"
#include "fixturebuilder.h"
#include "logbookmanager.h"
#include "preferences/enginepreferenceprovider.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "testenvironment.h"
#include "testmain.h"

using namespace FlySight;
using namespace FlySightTest;

class HarnessTest : public QObject {
    Q_OBJECT

private slots:
    void settingsAreIsolated();
    void logbookIsIsolated();
    void freshLogbookChangesFolder();
    void fs2BuilderBytes();
    void fs2BuilderOptions();
    void fs1BuilderBytes();
    void fileHelpersRoundTrip();
    void preferencesRegistered();
    void resetPreferencesKeepsLogbookFolder();
    void preferenceProviderAdapter();
};

void HarnessTest::settingsAreIsolated()
{
    TestEnvironment &env = TestEnvironment::instance();

    QCOMPARE(QCoreApplication::organizationName(), QStringLiteral("FlySightTests"));
    QCOMPARE(QCoreApplication::applicationName(), QStringLiteral("HarnessTest"));

    QSettings settings;
    QCOMPARE(settings.format(), QSettings::IniFormat);
    // On Apple platforms a default QSettings identifies the organization by
    // its domain when one is set; everywhere else by its name. Either way it
    // is the test organization, never the application's.
#ifdef Q_OS_DARWIN
    QCOMPARE(settings.organizationName(), QStringLiteral("tests.flysight.invalid"));
#else
    QCOMPARE(settings.organizationName(), QStringLiteral("FlySightTests"));
#endif

    const QString fileName = QDir::cleanPath(settings.fileName());
    QVERIFY2(fileName.startsWith(env.rootPath() + QLatin1Char('/'), Qt::CaseInsensitive),
             qPrintable(fileName));
    QVERIFY2(fileName.endsWith(QStringLiteral(".ini")), qPrintable(fileName));

    // The preferences the environment registered really went to that file.
    settings.sync();
    QVERIFY(QFileInfo::exists(fileName));
    QVERIFY(settings.contains(PreferenceKeys::GeneralLogbookFolder));
}

void HarnessTest::logbookIsIsolated()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();

    QVERIFY2(env.logbookDir().startsWith(env.rootPath() + QLatin1Char('/'), Qt::CaseInsensitive),
             qPrintable(env.logbookDir()));
    QVERIFY(env.logbookDir().endsWith(QStringLiteral("/FlySight Viewer/logbook")));
    QCOMPARE(env.sessionsDir(), env.logbookDir() + QStringLiteral("/sessions"));
    QCOMPARE(env.cacheDir(), env.logbookDir() + QStringLiteral("/cache"));
    QCOMPARE(env.indexPath(), env.logbookDir() + QStringLiteral("/index.json"));

    LogbookManager &logbook = LogbookManager::instance();
    logbook.initialize();
    QVERIFY(!logbook.hasIndexData());
    QVERIFY(logbook.hasDeferredScan());
    QVERIFY(logbook.scannedUuids().isEmpty());

    // initialize() created the sessions directory inside the temporary root,
    // and not the cache directory (the first calculation record creates it).
    QVERIFY(QDir(env.sessionsDir()).exists());
    QVERIFY(!QFileInfo::exists(env.cacheDir()));

    // reset() drops the state again.
    env.reopenLogbook();
    QVERIFY(!logbook.hasDeferredScan());
}

void HarnessTest::freshLogbookChangesFolder()
{
    TestEnvironment &env = TestEnvironment::instance();

    env.useFreshLogbook();
    const QString first = env.logbookFolder();
    env.useFreshLogbook();
    const QString second = env.logbookFolder();

    QVERIFY(first != second);
    QVERIFY(QDir(first).exists());
    QVERIFY(QDir(second).exists());
    QVERIFY(second.startsWith(env.rootPath() + QLatin1Char('/'), Qt::CaseInsensitive));

    const QString a = env.newTempDir(QStringLiteral("case"));
    const QString b = env.newTempDir(QStringLiteral("case"));
    QVERIFY(a != b);
    QVERIFY(QDir(a).exists());
    QVERIFY(a.startsWith(env.rootPath() + QLatin1Char('/'), Qt::CaseInsensitive));
}

void HarnessTest::fs2BuilderBytes()
{
    Fs2FileBuilder b;
    b.var("SESSION_ID", "abc")
     .sensor("IMU", {"time", "wx"}, {"s", "deg/s"})
     .sensor("CUSTOM", {"time", "foo"}, {})
     .row("IMU", "1.50,062.5")
     .row("CUSTOM", "2,7e-3");

    const QByteArray expected =
        "$FLYS,1\n"
        "$VAR,SESSION_ID,abc\n"
        "$COL,IMU,time,wx\n"
        "$UNIT,IMU,s,deg/s\n"
        "$COL,CUSTOM,time,foo\n"
        "$DATA\n"
        "$IMU,1.50,062.5\n"
        "$CUSTOM,2,7e-3\n";

    QCOMPARE(b.toBytes(), expected);
}

void HarnessTest::fs2BuilderOptions()
{
    Fs2FileBuilder b;
    b.flysVersion("2")
     .lineEnding("\r\n")
     .sensor("BARO", {"time", "pressure", "temperature"}, {"s", "Pa"})
     .rawHeaderLine("$BOGUS,1")
     .row("BARO", "1,2,3")
     .rawDataLine("not a row")
     .row("BARO", "4,5,6");

    const QByteArray expected =
        "$FLYS,2\r\n"
        "$COL,BARO,time,pressure,temperature\r\n"
        "$UNIT,BARO,s,Pa\r\n"
        "$BOGUS,1\r\n"
        "$DATA\r\n"
        "$BARO,1,2,3\r\n"
        "not a row\r\n"
        "$BARO,4,5,6\r\n";

    QCOMPARE(b.toBytes(), expected);

    const QByteArray expectedSensor =
        "$FLYS,1\n"
        "$VAR,FIRMWARE_VER,v2023.09.22\n"
        "$VAR,SESSION_ID,s1\n"
        "$VAR,DEVICE_ID,test-device\n"
        "$COL,IMU,time,wy,ax,wz,wx,temperature\n"
        "$UNIT,IMU,s,deg/s,g,deg/s,deg/s,deg C\n"
        "$COL,MAG,time,x,y,z,temperature\n"
        "$UNIT,MAG,s,gauss,gauss,gauss,deg C\n"
        "$DATA\n"
        "$IMU,3,-125,1,0,62.5,40\n"
        "$MAG,3,1,0,-0.5,40\n";

    QCOMPARE(Fixtures::sensorFile("s1").toBytes(), expectedSensor);
    QVERIFY(!Fixtures::trackFile().toBytes().contains("SCHEMA_VER"));
    QVERIFY(Fixtures::trackFile().toBytes().contains("$UNIT,GNSS,,deg,deg,m,m/s,m/s,m/s,m,m,m,\n"));
}

void HarnessTest::fs1BuilderBytes()
{
    Fs1FileBuilder b;
    b.columns({"time", "lat", "lon", "hMSL"})
     .units({"", "(deg)", "(deg)", "(m)"})
     .row("2024-01-01T12:00:00.00Z,45.5,-73.25,4000.000");

    const QByteArray expected =
        "time,lat,lon,hMSL\n"
        ",(deg),(deg),(m)\n"
        "2024-01-01T12:00:00.00Z,45.5,-73.25,4000.000\n";

    QCOMPARE(b.toBytes(), expected);

    // No unit line unless one was requested.
    Fs1FileBuilder bare;
    bare.columns({"time", "lat", "lon", "hMSL"});
    QCOMPARE(bare.toBytes(), QByteArray("time,lat,lon,hMSL\n"));
}

void HarnessTest::fileHelpersRoundTrip()
{
    TestEnvironment &env = TestEnvironment::instance();
    const QString path = env.newTempDir(QStringLiteral("files")) + QStringLiteral("/bytes.bin");

    const QByteArray contents("line one\r\nline two\n\0tail", 24);
    QVERIFY(writeFile(path, contents));
    QCOMPARE(readFileBytes(path), contents);

    // Overwrite truncates.
    QVERIFY(writeFile(path, "x"));
    QCOMPARE(readFileBytes(path), QByteArray("x"));

    QCOMPARE(readFileBytes(path + QStringLiteral(".missing")), QByteArray());
}

void HarnessTest::preferencesRegistered()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.resetPreferencesToDefaults();

    PreferencesManager &prefs = PreferencesManager::instance();
    QCOMPARE(prefs.getValue(PreferenceKeys::GeneralUnits).toString(), QStringLiteral("Metric"));
    QCOMPARE(prefs.getValue(PreferenceKeys::ImportGroundReferenceMode).toString(), QStringLiteral("Automatic"));
    QCOMPARE(prefs.getValue(PreferenceKeys::ImportFixedElevation).toDouble(), 0.0);
    QCOMPARE(prefs.getValue(PreferenceKeys::ImportDescentPauseSeconds).toDouble(), 30.0);
    QCOMPARE(prefs.getValue(PreferenceKeys::ImportHideOthersOnImport).toBool(), false);
    QCOMPARE(prefs.getValue(PreferenceKeys::AeroMass).toDouble(), 1.0);
    QCOMPARE(prefs.getValue(PreferenceKeys::AeroArea).toDouble(), 1.0);

    // The registered default for the logbook folder is under the temporary
    // root, never the user's Documents folder.
    QCOMPARE(prefs.getDefaultValue(PreferenceKeys::GeneralLogbookFolder).toString(),
             env.rootPath() + QStringLiteral("/logbook-0"));
    QVERIFY(env.logbookFolder().startsWith(env.rootPath() + QLatin1Char('/'), Qt::CaseInsensitive));
}

void HarnessTest::resetPreferencesKeepsLogbookFolder()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();
    const QString folder = env.logbookFolder();

    PreferencesManager &prefs = PreferencesManager::instance();
    prefs.setValue(PreferenceKeys::GeneralUnits, QStringLiteral("Imperial"));
    prefs.setValue(PreferenceKeys::AeroMass, 85.5);

    env.resetPreferencesToDefaults();

    QCOMPARE(prefs.getValue(PreferenceKeys::GeneralUnits).toString(), QStringLiteral("Metric"));
    QCOMPARE(prefs.getValue(PreferenceKeys::AeroMass).toDouble(), 1.0);
    QCOMPARE(env.logbookFolder(), folder);
}

// EnginePreferenceProvider: PreferencesManager as the engine's preference
// source, with synchronous invalidation on change.
void HarnessTest::preferenceProviderAdapter()
{
    TestEnvironment::instance().resetPreferencesToDefaults();
    PreferencesManager &prefs = PreferencesManager::instance();

    const QString key = PreferenceKeys::ImportDescentPauseSeconds;
    const QString output = QStringLiteral("_PAUSE");

    CalculationRegistry registry;
    EnginePreferenceProvider::install(registry);
    EnginePreferenceProvider::install(registry);    // idempotent
    QVERIFY(registry.preferenceProvider() != nullptr);

    CalculationDescriptor d;
    d.id = QStringLiteral("test.pause");
    d.inputs = {CalcInput::preference(key)};
    d.outputs = {DependencyKey::attribute(output)};
    d.compute = [key, output](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute(output, ctx.preference(key));
    };
    QVERIFY(registry.registerCalculation(d));

    {
        FakeSessionState state;
        CalculationEngine engine(&state, &registry);

        QList<QSet<DependencyKey>> delivered;
        engine.setInvalidationListener([&delivered](const QSet<DependencyKey> &keys) {
            delivered.append(keys);
        });

        QCOMPARE(engine.attribute(output).toDouble(), 30.0);
        QCOMPARE(engine.runCount(d.id), 1);

        // The change is delivered synchronously, inside setValue.
        prefs.setValue(key, 5.0);
        QCOMPARE(delivered.size(), 1);
        QCOMPARE(delivered.first(), QSet<DependencyKey>({DependencyKey::attribute(output)}));
        QCOMPARE(engine.attribute(output).toDouble(), 5.0);
        QCOMPARE(engine.runCount(d.id), 2);

        // A preference nothing declared invalidates nothing.
        prefs.setValue(PreferenceKeys::AeroMass, 85.5);
        QCOMPARE(delivered.size(), 1);

        // An unregistered key is "no such preference", without an assert.
        QVERIFY(!registry.preferenceProvider()->preferenceValue(QStringLiteral("no/such/key")).isValid());
    }

    // The private registry is about to die: detach it from PreferencesManager.
    EnginePreferenceProvider::uninstall(registry);
    QVERIFY(registry.preferenceProvider() == nullptr);
    prefs.setValue(key, 30.0);      // must not reach the dead adapter

    TestEnvironment::instance().resetPreferencesToDefaults();
}

FLYSIGHT_TEST_MAIN(HarnessTest)

#include "tst_harness.moc"
