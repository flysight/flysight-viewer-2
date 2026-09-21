// End-to-end workflows on the application's own code path (acceptance 19, as
// far as it can be shown without widgets): files on disk are imported through
// SessionImport::importFiles - the one call MainWindow makes - into a real
// SessionModel over a temporary logbook; rows and columns are read, markers
// and attributes are edited, the session is saved, and the logbook is reopened
// as the application would after a restart.
//
//   acceptance 19 - import -> rows -> columns -> marker / attribute edit -> save -> reopen
//   acceptance 5  - the model's save path writes what a cold export writes
//   acceptance 6  - a released logbook loads, is corrected once, and a save does
//                   not rescale, relabel, or stamp it
//   acceptance 18 - upgrading discards and recomputes cached gyro columns; an
//                   edit refreshes only the affected column
//
// This file deliberately never calls SessionModel::mergeSessions or
// DataImporter: every import goes through SessionImport::importFiles.
// Every expected value is a literal.

#include <memory>

#include <QtTest>

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSignalSpy>

#include "builtinfixture.h"
#include "dataexporter.h"
#include "engine/calculationengine.h"
#include "fixturebuilder.h"
#include "logbookcolumn.h"
#include "logbookmanager.h"
#include "logbookprobe.h"
#include "oraclecatalogue.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "sessiondata.h"
#include "sessionimport.h"
#include "sessionmodel.h"
#include "testenvironment.h"
#include "testmain.h"
#include "testutil.h"

using namespace FlySight;
using namespace FlySightTest;

Q_DECLARE_METATYPE(FlySight::DependencyKey)

namespace {

constexpr double T0 = DescentFixture::T0;
constexpr int kD = 0;   // column indices
constexpr int kX = 1;
constexpr int kG = 2;

using Outcome = MergeResult::Outcome;

const QString kId = QStringLiteral("descent");

QString onlySessionFile()
{
    const QStringList files = sessionCsvFiles();
    return files.size() == 1 ? TestEnvironment::instance().sessionsDir() + QLatin1Char('/') + files.first()
                             : QString();
}

QSet<QByteArray> lineSet(const QByteArray &bytes)
{
    QSet<QByteArray> lines;
    for (const QByteArray &line : bytes.split('\n')) {
        if (!line.isEmpty())
            lines.insert(line);
    }
    return lines;
}

// A session file as a released Viewer version wrote it: exporter order,
// normalized units (m/s^2, degC, T), no SCHEMA_VER, the lossy six-digit
// _IMPORT_TIME those versions produced, and a marker between two gyro samples.
const QByteArray kReleased =
    "$FLYS,1\n"
    "$VAR,FIRMWARE_VER,v2023.09.22\n"
    "$VAR,DEVICE_ID,test-device\n"
    "$VAR,SESSION_ID,rel\n"
    "$VAR,_DESCRIPTION,old jump\n"
    "$VAR,_IMPORT_TIME,1.7189e+09\n"
    "$VAR,_JUMPER_MASS,80\n"
    "$VAR,_M,1704110415\n"
    "$VAR,_PLANFORM_AREA,2\n"
    "$VAR,_WIND_E,0\n"
    "$VAR,_WIND_N,0\n"
    "$COL,MAG,time,x\n"
    "$UNIT,MAG,s,T\n"
    "$COL,IMU,time,wx,wy,wz,ax,temperature\n"
    "$UNIT,IMU,s,deg/s,deg/s,deg/s,m/s^2,degC\n"
    "$COL,TIME,time,tow,week\n"
    "$UNIT,TIME,s,s,\n"
    "$DATA\n"
    "$MAG,10,0.25\n"
    "$IMU,10,1,-125,0,9.80665,40\n"
    "$IMU,20,2,-125,0,9.80665,40\n"
    "$IMU,30,3,-125,0,9.80665,40\n"
    "$TIME,10,129610,2295\n"
    "$TIME,20,129620,2295\n"
    "$TIME,30,129630,2295\n";

} // namespace

class WorkflowTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void importEditSaveReopen();
    void warmModelSaveEqualsColdExport();
    void releasedLogbookUpgrade();

private:
    // Writes the descent fixture as a device folder and imports both files in
    // one batch, as a user dropping a folder does.
    SessionImport::BatchResult importDescent();
    // An application restart: a new model populated from index.json, the
    // column worker running.
    void reopen();
    QVariant cached(int row, int column) const { return m_model->rowAt(row).cachedValues.value(column); }

    std::unique_ptr<SessionModel> m_model;
};

void WorkflowTest::initTestCase()
{
    // As the application does: registrations are complete before initialize().
    TestEnvironment::instance().registerBuiltIns();
    PreferencesManager::instance().registerPreference(PreferenceKeys::LogbookColumnsVersion, 0);
    LogbookColumnStore::instance().setColumns({descriptionColumn(), exitTimeColumn(), gyroColumn()});
    qRegisterMetaType<FlySight::DependencyKey>();
}

void WorkflowTest::init()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();
    env.resetPreferencesToDefaults();
    LogbookManager::instance().initialize();
}

void WorkflowTest::cleanup()
{
    m_model.reset();
}

SessionImport::BatchResult WorkflowTest::importDescent()
{
    const QString folder = TestEnvironment::instance().newTempDir(QStringLiteral("card"))
                           + QStringLiteral("/24-01-01/12-00-00");
    if (!QDir().mkpath(folder))
        qFatal("could not create %s", qPrintable(folder));
    const QString track = folder + QStringLiteral("/TRACK.CSV");
    const QString sensor = folder + QStringLiteral("/SENSOR.CSV");
    if (!DescentFixture::trackFile(kId.toLatin1()).write(track)
        || !DescentFixture::sensorFile(kId.toLatin1()).write(sensor))
        qFatal("could not write the fixture files");
    return SessionImport::importFiles(*m_model, {track, sensor});
}

void WorkflowTest::reopen()
{
    LogbookManager &logbook = LogbookManager::instance();
    m_model.reset();
    TestEnvironment::instance().reopenLogbook();
    logbook.initialize();

    m_model = std::make_unique<SessionModel>();
    m_model->populateFromIndex(logbook.cachedColumnValues(LogbookColumnStore::instance().enabledColumns()),
                               logbook.lastAccessedMap());
    m_model->startColumnWorker();
}

// Acceptance 19
void WorkflowTest::importEditSaveReopen()
{
    m_model = std::make_unique<SessionModel>();

    // 1. import
    const SessionImport::BatchResult batch = importDescent();
    QCOMPARE(batch.files.size(), 2);
    QCOMPARE(batch.files.at(0).outcome, Outcome::Created);
    QCOMPARE(batch.files.at(1).outcome, Outcome::Merged);
    QVERIFY(SessionImport::failureMessage(batch.failures(), QString()).isEmpty());
    QCOMPARE(batch.importedSessionIds(), QStringList({kId}));
    QCOMPARE(m_model->rowCount(), 1);

    // 2. rows and columns: effective values under the recorded names, the
    // recorded values and unit text behind them
    {
        const SessionData &s = m_model->sessionRef(0);
        QCOMPARE(s.getAttribute("_DESCRIPTION").toString(), QStringLiteral("24-01-01/12-00-00"));
        QCOMPARE(s.getAttribute("_EXIT_TIME").toDouble(), T0 + 9.0);
        QCOMPARE(s.getAttribute("_SYNC_TIME").toDouble(), T0 + 9.0);
        QVERIFY(isNear(s.getMeasurement("IMU", "wx").value(0), 3.44064));
        QCOMPARE(s.sourceMeasurement("IMU", "wx"), QVector<double>({3.0, 6.0, 9.0}));
        QCOMPARE(s.sourceUnit("IMU", "ax"), QStringLiteral("g"));
        QVERIFY(!s.hasAttribute("SCHEMA_VER"));
    }

    // 3. a marker edit and attribute edits
    QSignalSpy spy(m_model.get(), &SessionModel::dependencyChanged);
    QVERIFY(m_model->updateAttribute(kId, "_EXIT_TIME", T0 + 12.0));
    QVERIFY(m_model->updateAttribute(kId, "_M", T0 + 19.0));
    QVERIFY(m_model->updateAttribute(kId, "_GROUND_ELEV", 50.0));
    QVERIFY(m_model->updateAttribute(kId, "_DESCRIPTION", QStringLiteral("workflow jump")));
    {
        const SessionData &s = m_model->sessionRef(0);
        QCOMPARE(s.getAttribute("_SYNC_TIME").toDouble(), T0 + 12.0);
        QVERIFY(isNear(s.getAttribute("_EXIT_TIME:GNSS/_time/hMSL").toDouble(), 3970.0));
        QCOMPARE(s.getMeasurement("GNSS", "z").value(0), 3950.0);
        QVERIFY(spyHasAttribute(spy, kId, "_SYNC_TIME"));
    }

    // 4. the save
    QVERIFY(waitForIdle(*m_model));
    const QString csvPath = onlySessionFile();
    QVERIFY(!csvPath.isEmpty());
    const QByteArray csv = readFileBytes(csvPath);
    QVERIFY(csv.contains("$VAR,_EXIT_TIME,1704110412\n"));
    QVERIFY(csv.contains("$VAR,_GROUND_ELEV,50\n"));
    QVERIFY(csv.contains("$VAR,_DESCRIPTION,workflow jump\n"));
    QVERIFY(csv.contains("$UNIT,IMU,s,deg/s,deg/s,deg/s,g,g,g,deg C\n"));
    QVERIFY(csv.contains("$IMU,10,3,4,0,0,0,1,40\n"));
    QVERIFY(!csv.contains("SCHEMA_VER"));
    QVERIFY(!csv.contains("3.44064"));      // never an effective value
    QVERIFY(!csv.contains("9.80665"));

    // 5. restart: the row is a stub and its columns come from index.json
    reopen();
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(m_model->rowCount(), 1);
    QVERIFY(!m_model->rowAt(0).isLoaded());
    QCOMPARE(cached(0, kD).toString(), QStringLiteral("workflow jump"));
    QCOMPARE(cached(0, kX).toDouble(), 1704110412.0);
    QVERIFY(isNear(cached(0, kG).toDouble(), 6.537216));    // (3 + 0.9 * 3) * 1.14688
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 0);
    QCOMPARE(m_model->columnWorkStats().valuesComputed, 0);

    // 6. the reloaded session
    const SessionData &s = m_model->sessionRef(0);
    QVERIFY(m_model->rowAt(0).isLoaded());
    QCOMPARE(s.getAttribute("_DESCRIPTION").toString(), QStringLiteral("workflow jump"));
    QCOMPARE(s.getAttribute("_EXIT_TIME").toDouble(), T0 + 12.0);
    QCOMPARE(s.getAttribute("_SYNC_TIME").toDouble(), T0 + 12.0);
    QVERIFY(isNear(s.getAttribute("_EXIT_TIME:GNSS/_time/hMSL").toDouble(), 3970.0));
    QCOMPARE(s.getMeasurement("GNSS", "z").value(0), 3950.0);
    QVERIFY(isNear(s.getMeasurement("IMU", "wx").value(0), 3.44064));
    QCOMPARE(s.sourceMeasurement("IMU", "wx"), QVector<double>({3.0, 6.0, 9.0}));
    QCOMPARE(s.sourceUnit("IMU", "ax"), QStringLiteral("g"));
    QVERIFY(!s.hasAttribute("SCHEMA_VER"));
    QVERIFY(s.calculationEngine().verifyAgainstFresh(oracleCatalogue()).isEmpty());
}

// Acceptance 5: the model's save path with a warm calculation cache writes
// exactly what the exporter writes for a cold copy of the session.
void WorkflowTest::warmModelSaveEqualsColdExport()
{
    m_model = std::make_unique<SessionModel>();
    const SessionImport::BatchResult batch = importDescent();
    QVERIFY(batch.failures().isEmpty());
    QCOMPARE(m_model->rowCount(), 1);

    // Warm: every catalogue name has been read
    {
        const SessionData &s = m_model->sessionRef(0);
        for (const DependencyKey &name : oracleCatalogue())
            oracleRead(s, name);
        QVERIFY(s.calculationEngine().totalRunCount() > 0);
    }

    QVERIFY(m_model->updateAttribute(kId, "_DESCRIPTION", QStringLiteral("w")));
    QVERIFY(waitForIdle(*m_model));

    const QString csvPath = onlySessionFile();
    QVERIFY(!csvPath.isEmpty());
    const QByteArray saved = readFileBytes(csvPath);
    QVERIFY(saved.contains("$VAR,_DESCRIPTION,w\n"));

    // A copy is state-only, hence cold.
    const SessionData cold(m_model->sessionRef(0));
    const std::optional<QByteArray> coldBytes = DataExporter::toBytes(cold);
    QVERIFY(coldBytes.has_value());
    QVERIFY(saved == *coldBytes);
}

// Acceptance 6 and 18, through SessionModel: a released session file together
// with a released index.json.
void WorkflowTest::releasedLogbookUpgrade()
{
    LogbookManager &logbook = LogbookManager::instance();
    const LogbookColumn d = descriptionColumn();
    const LogbookColumn g = gyroColumn();

    // Let the logbook create the index entry, with the cached values the
    // released version computed from uncorrected gyro data ...
    SessionData placeholder;
    placeholder.setAttribute("SESSION_ID", QStringLiteral("rel"));
    placeholder.setAttribute("DEVICE_ID", QStringLiteral("test-device"));
    placeholder.setSourceMeasurement("IMU", "time", {10.0}, "s");
    QVERIFY(logbook.saveSession(placeholder));
    logbook.setCachedValues(QStringLiteral("rel"), {{d, QStringLiteral("old jump")}, {g, 1.5}});
    QVERIFY(logbook.flushIndex());

    // ... then put the released file and a released index (no marker, no
    // environment fingerprint) in their place.
    const QString csvPath = onlySessionFile();
    QVERIFY(!csvPath.isEmpty());
    QVERIFY(writeFile(csvPath, kReleased));
    QJsonObject index = readIndex();
    QVERIFY(index.contains(QStringLiteral("calculationCompatibility")));
    index.remove(QStringLiteral("calculationCompatibility"));
    index.remove(QStringLiteral("calculationEnvironment"));
    QVERIFY(writeFile(TestEnvironment::instance().indexPath(), QJsonDocument(index).toJson()));
    QVERIFY(readFileBytes(TestEnvironment::instance().indexPath()).contains("1.5"));

    // The upgrade: start, let the column worker run
    reopen();
    QCOMPARE(m_model->rowCount(), 1);
    QVERIFY(waitForIdle(*m_model));

    QVERIFY(isNear(cached(0, kG).toDouble(), 1.72032));     // 1.5 * 1.14688, not 1.5
    QCOMPARE(cached(0, kD).toString(), QStringLiteral("old jump"));
    QCOMPARE(readFileBytes(csvPath), kReleased);            // no migration
    QCOMPARE(readIndex()[QStringLiteral("calculationCompatibility")].toInt(), 2);

    // An edit: only the description line changes, only that column is computed
    m_model->resetColumnWorkStats();
    QVERIFY(m_model->updateAttribute(QStringLiteral("rel"), "_DESCRIPTION", QStringLiteral("new text")));
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(m_model->columnWorkStats().valuesComputed, 1);

    const QByteArray saved = readFileBytes(csvPath);
    QSet<QByteArray> removed = lineSet(kReleased) - lineSet(saved);
    QSet<QByteArray> added = lineSet(saved) - lineSet(kReleased);
    QCOMPARE(removed, QSet<QByteArray>({"$VAR,_DESCRIPTION,old jump"}));
    QCOMPARE(added, QSet<QByteArray>({"$VAR,_DESCRIPTION,new text"}));
    QVERIFY(saved.contains("$VAR,_IMPORT_TIME,1.7189e+09\n"));
    QVERIFY(saved.contains("$UNIT,IMU,s,deg/s,deg/s,deg/s,m/s^2,degC\n"));
    QVERIFY(saved.contains("$UNIT,MAG,s,T\n"));
    QVERIFY(!saved.contains("SCHEMA_VER"));
    QVERIFY(isNear(cached(0, kG).toDouble(), 1.72032));
    QCOMPARE(cached(0, kD).toString(), QStringLiteral("new text"));
}

FLYSIGHT_TEST_MAIN(WorkflowTest)
#include "tst_workflow.moc"
