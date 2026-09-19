// The logbook column cache seen through SessionModel (spec 9.4):
//
//  - acceptance 18: an index.json without the calculation-compatibility marker
//    has its cached column values discarded and lazily recomputed (so the
//    gyro-derived columns of a released logbook come back corrected), and a
//    session edit refreshes only the columns it can affect - with a warm and
//    with a cold engine;
//  - an interrupted save can never leave a cached column that disagrees with
//    the session file on disk;
//  - a calculation-environment change (declared preference, registration)
//    discards the cached values of loaded AND unloaded rows without saving
//    anything.
//
// The "gyro session": TIME data with an exact fit (a = 1, b = T0), IMU/wx
// {1, 2, 3} deg/s at IMU/time {10, 20, 30}, no SCHEMA_VER. Column G reads
// IMU/wx at marker _M = T0 + 15: 1.5 as recorded, x 1.14688 = 1.72032 after
// the legacy-gyro correction. 1.5 is therefore the stale value a released
// index.json holds.

#include <memory>

#include <QtTest>

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSignalSpy>

#include "altitudemarkerfeature.h"
#include "builtinfixture.h"
#include "calculations/builtincalculations.h"
#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "fixturebuilder.h"
#include "logbookcolumn.h"
#include "logbookmanager.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "sessiondata.h"
#include "sessionmodel.h"
#include "testenvironment.h"
#include "testmain.h"

using namespace FlySight;
using namespace FlySightTest;

namespace {

constexpr double T0 = 1704110400.0;     // 2024-01-01T12:00:00Z

constexpr int kD = 0;   // column indices
constexpr int kG = 1;
constexpr int kE = 2;

bool isNear(double a, double b)
{
    return qAbs(a - b) <= 1e-9;
}

LogbookColumn descriptionColumn()
{
    LogbookColumn col;
    col.type = ColumnType::SessionAttribute;
    col.attributeKey = QStringLiteral("_DESCRIPTION");
    return col;
}

LogbookColumn gyroColumn()
{
    LogbookColumn col;
    col.type = ColumnType::MeasurementAtMarker;
    col.sensorID = QStringLiteral("IMU");
    col.measurementID = QStringLiteral("wx");
    col.measurementType = QStringLiteral("rotation");
    col.markerAttributeKey = QStringLiteral("_M");
    return col;
}

LogbookColumn exitTimeColumn()
{
    LogbookColumn col;
    col.type = ColumnType::SessionAttribute;
    col.attributeKey = QStringLiteral("_EXIT_TIME");
    return col;
}

SessionData gyroSession(const QString &id = QStringLiteral("g1"))
{
    SessionData s;
    s.setAttribute("SESSION_ID", id);
    s.setAttribute("DEVICE_ID", QStringLiteral("test-device"));
    s.setAttribute("_DESCRIPTION", QStringLiteral("first"));
    s.setAttribute("_M", T0 + 15.0);
    // What LogbookManager::loadSession would otherwise backfill
    s.setAttribute("_JUMPER_MASS", 80.0);
    s.setAttribute("_PLANFORM_AREA", 2.0);
    s.setAttribute("_WIND_N", 0.0);
    s.setAttribute("_WIND_E", 0.0);
    s.setSourceMeasurement("TIME", "time", {10.0, 20.0, 30.0}, "s");
    s.setSourceMeasurement("TIME", "tow", {129610.0, 129620.0, 129630.0}, "s");
    s.setSourceMeasurement("TIME", "week", {2295.0, 2295.0, 2295.0}, "");
    s.setSourceMeasurement("IMU", "time", {10.0, 20.0, 30.0}, "s");
    s.setSourceMeasurement("IMU", "wx", {1.0, 2.0, 3.0}, "deg/s");
    return s;
}

QJsonObject readIndex()
{
    return QJsonDocument::fromJson(readFileBytes(TestEnvironment::instance().indexPath())).object();
}

bool writeIndex(const QJsonObject &root)
{
    return writeFile(TestEnvironment::instance().indexPath(), QJsonDocument(root).toJson());
}

// The ephemeral id index.json uses for a column; empty when it has none.
QString indexColumnId(const QJsonObject &root, const LogbookColumn &col)
{
    const QJsonObject columns = root[QStringLiteral("columns")].toObject();
    for (auto it = columns.constBegin(); it != columns.constEnd(); ++it) {
        const QJsonObject def = it.value().toObject();
        const bool match = col.type == ColumnType::SessionAttribute
            ? def[QStringLiteral("type")].toString() == QLatin1String("SessionAttribute")
                  && def[QStringLiteral("attributeKey")].toString() == col.attributeKey
            : def[QStringLiteral("type")].toString() == QLatin1String("MeasurementAtMarker")
                  && def[QStringLiteral("sensorID")].toString() == col.sensorID
                  && def[QStringLiteral("measurementID")].toString() == col.measurementID
                  && def[QStringLiteral("markerAttributeKey")].toString() == col.markerAttributeKey;
        if (match)
            return it.key();
    }
    return QString();
}

// The value index.json holds for (session, column): Undefined when absent.
QJsonValue indexValue(const QJsonObject &root, const QString &sessionId, const LogbookColumn &col)
{
    const QString columnId = indexColumnId(root, col);
    if (columnId.isEmpty())
        return QJsonValue(QJsonValue::Undefined);
    return root[QStringLiteral("sessions")].toObject()[sessionId].toObject()
               [QStringLiteral("values")].toObject().value(columnId);
}

bool setIndexValue(QJsonObject &root, const QString &sessionId, const LogbookColumn &col, const QJsonValue &value)
{
    const QString columnId = indexColumnId(root, col);
    if (columnId.isEmpty())
        return false;
    QJsonObject sessions = root[QStringLiteral("sessions")].toObject();
    QJsonObject entry = sessions[sessionId].toObject();
    QJsonObject values = entry[QStringLiteral("values")].toObject();
    values[columnId] = value;
    entry[QStringLiteral("values")] = values;
    sessions[sessionId] = entry;
    root[QStringLiteral("sessions")] = sessions;
    return true;
}

QString sessionFilePath(const QString &sessionId)
{
    const QString uuid = readIndex()[QStringLiteral("sessions")].toObject()[sessionId].toObject()
                             [QStringLiteral("uuid")].toString();
    return TestEnvironment::instance().sessionsDir() + QLatin1Char('/') + uuid + QStringLiteral(".csv");
}

// Same as tst_session_model_engine: the QSettings array first, then a bump of
// the version preference, which makes an existing AltitudeMarkerManager refresh.
void writeAltitudes(const QList<int> &altitudes)
{
    {
        QSettings settings;
        settings.beginWriteArray(QStringLiteral("altitudeMarkers"), altitudes.size());
        for (int i = 0; i < altitudes.size(); ++i) {
            settings.setArrayIndex(i);
            settings.setValue(QStringLiteral("value"), altitudes.at(i));
        }
        settings.endArray();
    }

    PreferencesManager &prefs = PreferencesManager::instance();
    if (prefs.hasPreference(PreferenceKeys::AltitudeMarkersVersion)) {
        const int version = prefs.getValue(PreferenceKeys::AltitudeMarkersVersion).toInt();
        prefs.setValue(PreferenceKeys::AltitudeMarkersVersion, version + 1);
    }
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
    QStringList matching(const QString &fragment) const
    {
        QStringList result;
        for (const QString &w : std::as_const(g_warnings)) {
            if (w.contains(fragment))
                result.append(w);
        }
        return result;
    }

private:
    QtMessageHandler m_previous = nullptr;
};

// dataChanged emissions that span every row: the environment handler's one
// "everything may have changed" notification (the column worker reports rows
// one at a time).
int allRowsNotifications(const QSignalSpy &dataSpy, int rowCount)
{
    int count = 0;
    for (const QList<QVariant> &args : dataSpy) {
        if (args.at(0).toModelIndex().row() == 0 && args.at(1).toModelIndex().row() == rowCount - 1)
            ++count;
    }
    return count;
}

} // namespace

class ColumnCacheTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void upgradeDiscardsAndRecomputes();
    void currentMarkerKeepsStaleValue();

    void editRefreshesOnlyAffectedColumn_warm();
    void editRefreshesOnlyAffectedColumn_cold();
    void markerEditRefreshesDependents();
    void schemaEditRefreshesGyroColumn();
    void removeAttributeRefreshesDependents();
    void mergeRefreshesWrittenNamesOnly();
    void bulkEditOnStubComputesOneColumn();

    void interruptedSaveViaModel();
    void indexFlushWhileDirtyOmitsUnsaved();
    void newSessionIsNotIndexedBeforeItIsSaved();

    void preferenceChangeDiscardsUnloadedRows();
    void snapshotPreferenceDoesNotDiscard();
    void altitudeMarkerChangeDiscards();

    void saveFailureIsReported();
    void lineBreaksAreFlattenedAtEdit();

private:
    // A model whose rows were merged, saved, and indexed.
    void startWithLoadedSessions(const QList<SessionData> &sessions);
    // The same after an application restart: every row is a stub with all
    // its column values cached.
    void restartAsStubs();
    QVariant cached(int row, int column) const { return m_model->rowAt(row).cachedValues.value(column); }

    std::unique_ptr<SessionModel> m_model;
    std::unique_ptr<AltitudeMarkerManager> m_altitudes;
    QStringList m_registryBefore;

    const LogbookColumn m_d = descriptionColumn();
    const LogbookColumn m_g = gyroColumn();
    const LogbookColumn m_e = exitTimeColumn();
};

void ColumnCacheTest::initTestCase()
{
    // As the application does: registrations are complete before initialize().
    TestEnvironment::instance().registerBuiltIns();
    PreferencesManager::instance().registerPreference(PreferenceKeys::LogbookColumnsVersion, 0);
    LogbookColumnStore::instance().setColumns({m_d, m_g, m_e});
}

void ColumnCacheTest::init()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();
    env.resetPreferencesToDefaults();
    LogbookManager::instance().initialize();
    m_registryBefore = CalculationRegistry::instance().registeredIds();
}

void ColumnCacheTest::cleanup()
{
    m_altitudes.reset();
    writeAltitudes({});
    m_model.reset();
    QCOMPARE(CalculationRegistry::instance().registeredIds(), m_registryBefore);
    QCOMPARE(CalculationRegistry::instance().enrolledEngineCount(), 0);
}

void ColumnCacheTest::startWithLoadedSessions(const QList<SessionData> &sessions)
{
    m_model = std::make_unique<SessionModel>();
    m_model->mergeSessions(sessions);
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(!LogbookManager::instance().indexNeedsFlush());
}

void ColumnCacheTest::restartAsStubs()
{
    LogbookManager &logbook = LogbookManager::instance();
    m_model.reset();
    TestEnvironment::instance().reopenLogbook();
    logbook.initialize();

    m_model = std::make_unique<SessionModel>();
    m_model->populateFromIndex(logbook.cachedColumnValues(LogbookColumnStore::instance().enabledColumns()),
                               logbook.lastAccessedMap());
}

// Acceptance 18: a missing calculation-compatibility marker discards the
// cached column values; they are recomputed lazily, the index is rewritten
// with the marker, and no session file is touched. This is how the
// gyro-derived columns of a released logbook get corrected.
void ColumnCacheTest::upgradeDiscardsAndRecomputes()
{
    LogbookManager &logbook = LogbookManager::instance();
    startWithLoadedSessions({gyroSession()});
    QVERIFY(isNear(indexValue(readIndex(), "g1", m_g).toDouble(), 1.72032));

    const QString csvPath = sessionFilePath(QStringLiteral("g1"));
    const QByteArray csvBytes = readFileBytes(csvPath);
    QVERIFY(csvBytes.contains("$IMU,10,1\n"));

    // index.json as a released version left it: no marker, uncorrected gyro value
    m_model.reset();
    QJsonObject root = readIndex();
    root.remove(QStringLiteral("calculationCompatibility"));
    root.remove(QStringLiteral("calculationEnvironment"));
    QVERIFY(setIndexValue(root, QStringLiteral("g1"), m_g, 1.5));
    QVERIFY(writeIndex(root));

    restartAsStubs();
    QVERIFY(logbook.cachedValuesDiscardedOnLoad());
    QCOMPARE(m_model->rowCount(), 1);
    QVERIFY(!m_model->rowAt(0).isLoaded());
    QVERIFY(m_model->rowAt(0).cachedValues.isEmpty());

    m_model->startColumnWorker();
    QVERIFY(waitForIdle(*m_model));

    QVERIFY(isNear(cached(0, kG).toDouble(), 1.72032));
    QCOMPARE(cached(0, kD).toString(), QStringLiteral("first"));
    QCOMPARE(m_model->rowAt(0).cachedValues.size(), 3);
    QVERIFY(!m_model->rowAt(0).isLoaded());
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 1);
    QCOMPARE(m_model->columnWorkStats().valuesComputed, 3);

    const QJsonObject rewritten = readIndex();
    QCOMPARE(rewritten[QStringLiteral("calculationCompatibility")].toInt(), 1);
    QCOMPARE(rewritten[QStringLiteral("calculationEnvironment")].toString(), calculationEnvironmentFingerprint());
    QVERIFY(isNear(indexValue(rewritten, "g1", m_g).toDouble(), 1.72032));
    QCOMPARE(indexValue(rewritten, "g1", m_d).toString(), QStringLiteral("first"));

    QCOMPARE(readFileBytes(csvPath), csvBytes);
}

// The control for the test above: with the current marker and environment the
// planted value is trusted, and nothing is loaded.
void ColumnCacheTest::currentMarkerKeepsStaleValue()
{
    startWithLoadedSessions({gyroSession()});

    m_model.reset();
    QJsonObject root = readIndex();
    QVERIFY(setIndexValue(root, QStringLiteral("g1"), m_g, 1.5));
    QVERIFY(writeIndex(root));

    restartAsStubs();
    QVERIFY(!LogbookManager::instance().cachedValuesDiscardedOnLoad());
    m_model->startColumnWorker();
    QVERIFY(waitForIdle(*m_model));

    QCOMPARE(cached(0, kG).toDouble(), 1.5);
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 0);
    QCOMPARE(m_model->columnWorkStats().valuesComputed, 0);
}

// Acceptance 18: a session edit refreshes only the affected columns.
void ColumnCacheTest::editRefreshesOnlyAffectedColumn_warm()
{
    startWithLoadedSessions({gyroSession()});
    QVERIFY(m_model->rowAt(0).isLoaded());
    QVERIFY(isNear(cached(0, kG).toDouble(), 1.72032));

    m_model->resetColumnWorkStats();
    m_model->sessionRef(0).calculationEngine().resetRunCounts();

    QVERIFY(m_model->updateAttribute("g1", "_DESCRIPTION", QStringLiteral("second")));
    QVERIFY(!m_model->rowAt(0).cachedValues.contains(kD));
    QVERIFY(m_model->rowAt(0).cachedValues.contains(kG));
    QVERIFY(m_model->rowAt(0).cachedValues.contains(kE));
    QVERIFY(waitForIdle(*m_model));

    QCOMPARE(m_model->columnWorkStats().valuesComputed, 1);
    QCOMPARE(m_model->columnWorkStats().calculationRuns, 0);
    QCOMPARE(m_model->sessionRef(0).calculationEngine().totalRunCount(), 0);
    QCOMPARE(cached(0, kD).toString(), QStringLiteral("second"));

    const QJsonObject root = readIndex();
    QCOMPARE(indexValue(root, "g1", m_d).toString(), QStringLiteral("second"));
    QVERIFY(isNear(indexValue(root, "g1", m_g).toDouble(), 1.72032));
    QVERIFY(readFileBytes(sessionFilePath("g1")).contains("$VAR,_DESCRIPTION,second\n"));
    QVERIFY(!LogbookManager::instance().hasUnsavedColumns("g1"));
}

// Acceptance 18, with a cold engine: a session loaded from a stub has nothing
// cached, and the edit of the description still does not re-run the
// conversion, the time fit, or the interpolation behind column G.
void ColumnCacheTest::editRefreshesOnlyAffectedColumn_cold()
{
    startWithLoadedSessions({gyroSession()});
    restartAsStubs();
    QCOMPARE(m_model->rowAt(0).cachedValues.size(), 3);

    SessionData &session = m_model->sessionRef(0);      // loads; the engine is cold
    QCOMPARE(session.calculationEngine().totalRunCount(), 0);
    m_model->resetColumnWorkStats();

    QVERIFY(m_model->updateAttribute("g1", "_DESCRIPTION", QStringLiteral("third")));
    QVERIFY(waitForIdle(*m_model));

    QCOMPARE(m_model->columnWorkStats().valuesComputed, 1);
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 0);
    CalculationEngine &engine = m_model->sessionRef(0).calculationEngine();
    QCOMPARE(engine.runCountForInstance(QStringLiteral("builtin.conversion.default#IMU/wx")), 0);
    QCOMPARE(engine.runCountForInstance(QStringLiteral("builtin.conversion.schema#IMU/wx")), 0);
    QCOMPARE(engine.runCount(QStringLiteral("builtin.conversion.schema")), 0);
    QCOMPARE(engine.runCount(QStringLiteral("builtin.time.fit")), 0);
    QCOMPARE(engine.totalRunCount(), 0);

    QVERIFY(isNear(cached(0, kG).toDouble(), 1.72032));
    const QJsonObject root = readIndex();
    QVERIFY(isNear(indexValue(root, "g1", m_g).toDouble(), 1.72032));
    QCOMPARE(indexValue(root, "g1", m_d).toString(), QStringLiteral("third"));
}

void ColumnCacheTest::markerEditRefreshesDependents()
{
    startWithLoadedSessions({gyroSession()});
    m_model->resetColumnWorkStats();

    QVERIFY(m_model->updateAttribute("g1", "_M", T0 + 25.0));
    QVERIFY(!m_model->rowAt(0).cachedValues.contains(kG));
    QVERIFY(m_model->rowAt(0).cachedValues.contains(kD));
    QVERIFY(waitForIdle(*m_model));

    // 2.5 x 1.14688
    QVERIFY(isNear(cached(0, kG).toDouble(), 2.86720));
    QCOMPARE(m_model->columnWorkStats().valuesComputed, 1);
    QVERIFY(isNear(indexValue(readIndex(), "g1", m_g).toDouble(), 2.86720));
}

void ColumnCacheTest::schemaEditRefreshesGyroColumn()
{
    startWithLoadedSessions({gyroSession()});
    m_model->resetColumnWorkStats();

    QVERIFY(m_model->updateAttribute("g1", "SCHEMA_VER", QStringLiteral("2")));
    QVERIFY(!m_model->rowAt(0).cachedValues.contains(kG));
    QVERIFY(m_model->rowAt(0).cachedValues.contains(kD));
    QVERIFY(m_model->rowAt(0).cachedValues.contains(kE));
    QVERIFY(waitForIdle(*m_model));

    QCOMPARE(cached(0, kG).toDouble(), 1.5);    // schema 2: as recorded, exactly
    QCOMPARE(m_model->columnWorkStats().valuesComputed, 1);
    QCOMPARE(indexValue(readIndex(), "g1", m_g).toDouble(), 1.5);
}

void ColumnCacheTest::removeAttributeRefreshesDependents()
{
    startWithLoadedSessions({gyroSession()});
    m_model->resetColumnWorkStats();

    QVERIFY(m_model->removeAttribute("g1", "_M"));
    QVERIFY(waitForIdle(*m_model));

    QVERIFY(m_model->rowAt(0).cachedValues.contains(kG));
    QVERIFY(!cached(0, kG).isValid());          // computed: no value
    QCOMPARE(m_model->columnWorkStats().valuesComputed, 1);
    QVERIFY(indexValue(readIndex(), "g1", m_g).isNull());
    QCOMPARE(indexValue(readIndex(), "g1", m_d).toString(), QStringLiteral("first"));
}

// A merge into a loaded row invalidates the columns that can depend on the
// names it wrote, and no longer recomputes every loaded row.
void ColumnCacheTest::mergeRefreshesWrittenNamesOnly()
{
    startWithLoadedSessions({gyroSession(QStringLiteral("g1")), gyroSession(QStringLiteral("g2"))});
    m_model->resetColumnWorkStats();

    // Phase 6 merge rules: a Viewer attribute the session already has is kept
    // (_DESCRIPTION stays "first"), an absent one is added. _EXIT_TIME feeds
    // column E and nothing else.
    SessionData incoming;
    incoming.setAttribute("SESSION_ID", QStringLiteral("g1"));
    incoming.setAttribute("_DESCRIPTION", QStringLiteral("merged"));
    incoming.setAttribute("_EXIT_TIME", T0 + 12.0);
    m_model->mergeSessions({incoming});

    const int row = m_model->getSessionRow("g1");
    const int other = m_model->getSessionRow("g2");
    QVERIFY(!m_model->rowAt(row).cachedValues.contains(kE));
    QVERIFY(m_model->rowAt(row).cachedValues.contains(kD));
    QVERIFY(m_model->rowAt(row).cachedValues.contains(kG));
    QCOMPARE(m_model->rowAt(other).cachedValues.size(), 3);
    QVERIFY(LogbookManager::instance().hasUnsavedColumns("g1"));
    QVERIFY(!LogbookManager::instance().hasUnsavedColumns("g2"));

    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(m_model->columnWorkStats().valuesComputed, 1);
    QCOMPARE(indexValue(readIndex(), "g1", m_e).toDouble(), 1704110412.0);
    QCOMPARE(indexValue(readIndex(), "g1", m_d).toString(), QStringLiteral("first"));
    QCOMPARE(indexValue(readIndex(), "g2", m_d).toString(), QStringLiteral("first"));

    // Source data: IMU/wx feeds G and nothing else
    m_model->resetColumnWorkStats();
    SessionData sensor;
    sensor.setAttribute("SESSION_ID", QStringLiteral("g1"));
    sensor.setSourceMeasurement("IMU", "wx", {10.0, 20.0, 30.0}, "deg/s");
    m_model->mergeSessions({sensor});
    QVERIFY(!m_model->rowAt(row).cachedValues.contains(kG));
    QVERIFY(m_model->rowAt(row).cachedValues.contains(kD));
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(m_model->columnWorkStats().valuesComputed, 1);
    QVERIFY(isNear(indexValue(readIndex(), "g1", m_g).toDouble(), 17.2032));
}

void ColumnCacheTest::bulkEditOnStubComputesOneColumn()
{
    startWithLoadedSessions({gyroSession()});
    restartAsStubs();
    m_model->resetColumnWorkStats();

    m_model->startBulkEdit({0}, kD, QStringLiteral("bulk"));
    QVERIFY(waitForIdle(*m_model));

    QVERIFY(!m_model->rowAt(0).isLoaded());
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 1);
    QCOMPARE(m_model->columnWorkStats().valuesComputed, 1);
    QCOMPARE(m_model->columnWorkStats().calculationRuns, 0);
    QCOMPARE(cached(0, kD).toString(), QStringLiteral("bulk"));
    QVERIFY(isNear(cached(0, kG).toDouble(), 1.72032));

    QVERIFY(readFileBytes(sessionFilePath("g1")).contains("$VAR,_DESCRIPTION,bulk\n"));
    const QJsonObject root = readIndex();
    QCOMPARE(indexValue(root, "g1", m_d).toString(), QStringLiteral("bulk"));
    QVERIFY(isNear(indexValue(root, "g1", m_g).toDouble(), 1.72032));
    QVERIFY(!LogbookManager::instance().hasUnsavedColumns("g1"));
}

// Spec 9.4: an interrupted save cannot leave cached columns that disagree
// with the saved session file. Here the process "dies" right after the
// session file was written, before the index flush that normally follows.
void ColumnCacheTest::interruptedSaveViaModel()
{
    LogbookManager &logbook = LogbookManager::instance();
    startWithLoadedSessions({gyroSession()});

    QVERIFY(m_model->updateAttribute("g1", "_DESCRIPTION", QStringLiteral("crash")));
    // No event loop: what the SaveTask step does, without its completion flush
    QVERIFY(logbook.saveSession(m_model->sessionRef(0)));

    const QJsonObject snapshot = readIndex();
    QVERIFY(indexValue(snapshot, "g1", m_d).isUndefined());
    QVERIFY(isNear(indexValue(snapshot, "g1", m_g).toDouble(), 1.72032));
    const QString csvPath = sessionFilePath("g1");
    QVERIFY(readFileBytes(csvPath).contains("$VAR,_DESCRIPTION,crash\n"));

    restartAsStubs();
    QVERIFY(!logbook.cachedValuesDiscardedOnLoad());
    QVERIFY(!m_model->rowAt(0).cachedValues.contains(kD));
    QVERIFY(isNear(cached(0, kG).toDouble(), 1.72032));

    m_model->startColumnWorker();
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(m_model->columnWorkStats().valuesComputed, 1);
    QCOMPARE(cached(0, kD).toString(), QStringLiteral("crash"));
    QCOMPARE(indexValue(readIndex(), "g1", m_d).toString(), QStringLiteral("crash"));
    QVERIFY(readFileBytes(csvPath).contains("$VAR,_DESCRIPTION,crash\n"));
}

// The opposite order: an index flush (what a ColumnTask completion does)
// while the edited row has not been saved yet must not publish the new value
// next to the old session file.
void ColumnCacheTest::indexFlushWhileDirtyOmitsUnsaved()
{
    LogbookManager &logbook = LogbookManager::instance();
    startWithLoadedSessions({gyroSession()});

    QVERIFY(m_model->updateAttribute("g1", "_DESCRIPTION", QStringLiteral("crash")));
    QVERIFY(m_model->rowAt(0).dirty);
    QVERIFY(logbook.flushIndex());

    const QJsonObject root = readIndex();
    QVERIFY(indexValue(root, "g1", m_d).isUndefined());
    QVERIFY(isNear(indexValue(root, "g1", m_g).toDouble(), 1.72032));
    QVERIFY(readFileBytes(sessionFilePath("g1")).contains("$VAR,_DESCRIPTION,first\n"));

    // After the restart the old file and the (absent -> recomputed) value agree
    restartAsStubs();
    m_model->startColumnWorker();
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(cached(0, kD).toString(), QStringLiteral("first"));
}

// A new session's values are not published before its file exists.
void ColumnCacheTest::newSessionIsNotIndexedBeforeItIsSaved()
{
    LogbookManager &logbook = LogbookManager::instance();
    m_model = std::make_unique<SessionModel>();
    m_model->mergeSessions({gyroSession()});
    QVERIFY(m_model->rowAt(0).dirty);
    QVERIFY(logbook.hasUnsavedColumns("g1"));

    QVERIFY(waitForIdle(*m_model));
    QVERIFY(!m_model->rowAt(0).dirty);
    QVERIFY(!logbook.hasUnsavedColumns("g1"));
    QCOMPARE(m_model->columnWorkStats().valuesComputed, 3);
    QVERIFY(isNear(indexValue(readIndex(), "g1", m_g).toDouble(), 1.72032));
}

// A declared preference is part of the calculation environment. Changing it
// discards the cached values of every row - the unloaded ones too, which no
// engine invalidation can reach - and saves nothing.
void ColumnCacheTest::preferenceChangeDiscardsUnloadedRows()
{
    LogbookManager &logbook = LogbookManager::instance();
    startWithLoadedSessions({DescentFixture::load("s1"), DescentFixture::load("s2")});
    restartAsStubs();
    const int row1 = m_model->getSessionRow("s1");
    const int row2 = m_model->getSessionRow("s2");
    m_model->sessionRef(row1);      // s1 loaded, s2 stays a stub
    QVERIFY(m_model->rowAt(row1).isLoaded());
    QVERIFY(!m_model->rowAt(row2).isLoaded());
    QCOMPARE(cached(row2, kE).toDouble(), T0 + 9.0);

    const QByteArray csv1 = readFileBytes(sessionFilePath("s1"));
    const QByteArray csv2 = readFileBytes(sessionFilePath("s2"));
    const QString oldEnvironment = logbook.cacheEnvironment();
    QCOMPARE(oldEnvironment, calculationEnvironmentFingerprint());
    m_model->resetColumnWorkStats();

    PreferencesManager::instance().setValue(PreferenceKeys::ImportDescentPauseSeconds, 5.0);
    m_model->flushPendingInvalidations();

    const QString newEnvironment = calculationEnvironmentFingerprint();
    QVERIFY(newEnvironment != oldEnvironment);
    QCOMPARE(logbook.cacheEnvironment(), newEnvironment);
    QVERIFY(m_model->rowAt(row1).cachedValues.isEmpty());
    QVERIFY(m_model->rowAt(row2).cachedValues.isEmpty());
    QVERIFY(logbook.cachedValuesForSession("s2").isEmpty());

    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(m_model->rowAt(row1).cachedValues.size(), 3);
    QCOMPARE(m_model->rowAt(row2).cachedValues.size(), 3);
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 1);     // s2; s1 was in memory
    QCOMPARE(m_model->columnWorkStats().valuesComputed, 6);
    QVERIFY(cached(row2, kE).isValid());

    QCOMPARE(readIndex()[QStringLiteral("calculationEnvironment")].toString(), newEnvironment);
    QVERIFY(indexValue(readIndex(), "s2", m_e).isDouble());

    // Persistent state did not change: nothing dirty, nothing unsaved, no file rewritten
    QVERIFY(!m_model->rowAt(row1).dirty);
    QVERIFY(!m_model->rowAt(row2).dirty);
    QVERIFY(!logbook.hasUnsavedColumns("s1"));
    QVERIFY(!logbook.hasUnsavedColumns("s2"));
    QCOMPARE(readFileBytes(sessionFilePath("s1")), csv1);
    QCOMPARE(readFileBytes(sessionFilePath("s2")), csv2);

    // Back again: another change of environment
    PreferencesManager::instance().setValue(PreferenceKeys::ImportDescentPauseSeconds, 30.0);
    m_model->flushPendingInvalidations();
    QCOMPARE(logbook.cacheEnvironment(), oldEnvironment);
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(cached(row2, kE).toDouble(), T0 + 9.0);
}

// Snapshotted preferences (copied into the session at import) are not inputs
// of any calculation and not part of the environment.
void ColumnCacheTest::snapshotPreferenceDoesNotDiscard()
{
    startWithLoadedSessions({gyroSession()});
    restartAsStubs();
    const QString environment = calculationEnvironmentFingerprint();
    m_model->resetColumnWorkStats();

    PreferencesManager::instance().setValue(PreferenceKeys::AeroMass, 90.0);
    m_model->flushPendingInvalidations();
    QVERIFY(waitForIdle(*m_model));

    QCOMPARE(m_model->columnWorkStats().valuesComputed, 0);
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 0);
    QCOMPARE(calculationEnvironmentFingerprint(), environment);
    QCOMPARE(LogbookManager::instance().cacheEnvironment(), environment);
    QCOMPARE(m_model->rowAt(0).cachedValues.size(), 3);
}

// Registrations are part of the environment. One refresh of the altitude
// markers is several registry changes and one environment check.
void ColumnCacheTest::altitudeMarkerChangeDiscards()
{
    LogbookManager &logbook = LogbookManager::instance();
    startWithLoadedSessions({gyroSession(QStringLiteral("g1")), gyroSession(QStringLiteral("g2"))});
    restartAsStubs();
    QCOMPARE(m_model->rowCount(), 2);

    m_altitudes = std::make_unique<AltitudeMarkerManager>(m_model.get());
    PreferencesManager::instance().setValue(PreferenceKeys::AltitudeMarkersUnits, QStringLiteral("Metric"));
    m_altitudes->registerAll();     // nothing configured: registers nothing
    m_model->flushPendingInvalidations();
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(CalculationRegistry::instance().registeredIds(), m_registryBefore);
    const QString oldEnvironment = logbook.cacheEnvironment();

    m_model->resetColumnWorkStats();
    QSignalSpy dataSpy(m_model.get(), &SessionModel::dataChanged);

    writeAltitudes({500, 1000, 1500});      // three registrations in one refresh
    QCOMPARE(CalculationRegistry::instance().registeredIds().size(), m_registryBefore.size() + 3);
    QCOMPARE(allRowsNotifications(dataSpy, 2), 0);

    QCoreApplication::processEvents();      // the queued, coalesced check
    QCOMPARE(allRowsNotifications(dataSpy, 2), 1);
    QVERIFY(logbook.cacheEnvironment() != oldEnvironment);
    QCOMPARE(logbook.cacheEnvironment(), calculationEnvironmentFingerprint());

    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(allRowsNotifications(dataSpy, 2), 1);
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 2);
    QCOMPARE(m_model->columnWorkStats().valuesComputed, 6);
    QVERIFY(isNear(cached(0, kG).toDouble(), 1.72032));
    QVERIFY(isNear(cached(1, kG).toDouble(), 1.72032));
    QCOMPARE(readIndex()[QStringLiteral("calculationEnvironment")].toString(), logbook.cacheEnvironment());
}

// A failed save is reported once with the exporter's reason, is not retried,
// leaves the previous file intact, and keeps the affected values out of the index.
void ColumnCacheTest::saveFailureIsReported()
{
    startWithLoadedSessions({gyroSession()});
    const QString csvPath = sessionFilePath("g1");
    const QByteArray csvBytes = readFileBytes(csvPath);

    // Make the loaded session ragged behind the model's back
    m_model->sessionRef(0).setMeasurement("IMU", "wx", {1.0, 2.0});

    WarningCollector warnings;
    QVERIFY(m_model->updateAttribute("g1", "_DESCRIPTION", QStringLiteral("unsaved")));
    QVERIFY(waitForIdle(*m_model));     // goes idle: no retry loop

    const QStringList reported = warnings.matching(QStringLiteral("was not saved"));
    QCOMPARE(reported.size(), 1);
    QVERIFY2(reported.first().contains(QStringLiteral("g1")), qPrintable(reported.first()));
    QVERIFY2(reported.first().contains(
                 QStringLiteral("Sensor 'IMU' has columns of unequal length (time: 3, wx: 2)")),
             qPrintable(reported.first()));

    QVERIFY(!m_model->rowAt(0).dirty);
    QCOMPARE(readFileBytes(csvPath), csvBytes);

    // index.json still describes the file on disk: no value for the edited column
    QVERIFY(LogbookManager::instance().hasUnsavedColumns("g1"));
    const QJsonObject root = readIndex();
    QVERIFY(indexValue(root, "g1", m_d).isUndefined());
    QVERIFY(isNear(indexValue(root, "g1", m_g).toDouble(), 1.72032));
}

// A session file is a line format: text edits are flattened on entry, so that
// memory equals what a reload gives.
void ColumnCacheTest::lineBreaksAreFlattenedAtEdit()
{
    startWithLoadedSessions({gyroSession()});

    QVERIFY(m_model->updateAttribute("g1", "_DESCRIPTION", QStringLiteral("a\nb")));
    QCOMPARE(m_model->sessionRef(0).storedAttribute("_DESCRIPTION"), QVariant(QStringLiteral("a b")));
    // The same text again is no change
    QVERIFY(!m_model->updateAttribute("g1", "_DESCRIPTION", QStringLiteral("a\r\nb")));

    QVERIFY(m_model->setData(m_model->index(0, kD), QStringLiteral("c\r\nd"), Qt::EditRole));
    QCOMPARE(m_model->sessionRef(0).storedAttribute("_DESCRIPTION"), QVariant(QStringLiteral("c d")));

    QVERIFY(waitForIdle(*m_model));
    QVERIFY(readFileBytes(sessionFilePath("g1")).contains("$VAR,_DESCRIPTION,c d\n"));
    QCOMPARE(indexValue(readIndex(), "g1", m_d).toString(), QStringLiteral("c d"));
}

FLYSIGHT_TEST_MAIN(ColumnCacheTest)
#include "tst_column_cache.moc"
