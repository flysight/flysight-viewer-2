// The logbook column cache seen through SessionModel:
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
#include "logbookprobe.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "sessiondata.h"
#include "sessionmodel.h"
#include "testenvironment.h"
#include "testmain.h"
#include "testutil.h"

using namespace FlySight;
using namespace FlySightTest;

namespace {

constexpr double T0 = 1704110400.0;     // 2024-01-01T12:00:00Z

constexpr int kD = 0;   // column indices
constexpr int kG = 1;
constexpr int kE = 2;

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
    void bulkEditFollowsSessionsAcrossSort();
    void bulkEditSkipsRemovedSession();
    void bulkEditFollowsSessionsAcrossMerge();
    void bulkEditFollowsIdentityStubRemap();
    void bulkEditSurvivesDisabledColumn();

    void interruptedSaveViaModel();
    void indexFlushWhileDirtyOmitsUnsaved();
    void newSessionIsNotIndexedBeforeItIsSaved();

    void preferenceChangeDiscardsUnloadedRows();
    void snapshotPreferenceDoesNotDiscard();
    void altitudeMarkerChangeDiscards();
    void altitudeMarkerRemovalDiscardsStubValues();

    void saveFailureIsReported();
    void failedSaveRowIsNotEvicted();
    void newEditRetriesFailedSave();
    void lineBreaksAreFlattenedAtEdit();

    void uniqueColumnsKeepsFirstOccurrence();
    void storeCollapsesDuplicatesOnLoad();
    void indexValueReachesEveryColumnOfItsDefinition();
    void duplicateColumnCausesNoWorkAfterRestart();

private:
    // A model whose rows were merged, saved, and indexed.
    void startWithLoadedSessions(const QList<SessionData> &sessions);
    // The same after an application restart: every row is a stub with all
    // its column values cached.
    void restartAsStubs();
    QVariant cached(int row, int column) const { return m_model->rowAt(row).cachedValues.value(column); }
    // Rows [sA, sB, sC] with descriptions "m", "a", "z", so that a sort by
    // description reorders them; sA and sB are stubs, sC is loaded.
    void startWithMixedRows();
    QString descriptionOf(const QString &sessionId) const;     // in memory
    bool fileHasDescription(const QString &sessionId, const char *text) const;

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
    LogbookColumnStore::instance().setColumns({m_d, m_g, m_e});     // altitudeMarkerRemovalDiscardsStubValues adds one
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
    QCOMPARE(rewritten[QStringLiteral("calculationCompatibility")].toInt(), 2);
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

    // Merge rule: a Viewer ("_") attribute the session already has is kept
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

// ---- a queued bulk edit names sessions and the attribute, not indices ----
//
// The edit is queued, and the rows (or columns) change before the idle
// scheduler processes it.

namespace {

SessionData describedSession(const QString &id, const QString &description)
{
    SessionData s = gyroSession(id);
    s.setAttribute("_DESCRIPTION", description);
    return s;
}

// The bulk edit task's share of the scheduler's signals
struct BulkEditSignals {
    explicit BulkEditSignals(SessionModel &model)
        : active(&model.scheduler(), &IdleScheduler::activeTaskChanged)
        , progress(&model.scheduler(), &IdleScheduler::progressChanged)
    {}
    int activations() const
    {
        int count = 0;
        for (const QList<QVariant> &args : active)
            count += args.at(0).toInt() == SessionModel::BulkEditTask ? 1 : 0;
        return count;
    }
    // {remaining, total} of the last report, {-1, -1} if there was none
    QPair<int, int> lastProgress() const
    {
        QPair<int, int> last(-1, -1);
        for (const QList<QVariant> &args : progress) {
            if (args.at(0).toInt() == SessionModel::BulkEditTask)
                last = {args.at(1).toInt(), args.at(2).toInt()};
        }
        return last;
    }
    QSignalSpy active;
    QSignalSpy progress;
};

} // namespace

void ColumnCacheTest::startWithMixedRows()
{
    startWithLoadedSessions({describedSession("sA", "m"), describedSession("sB", "a"), describedSession("sC", "z")});
    restartAsStubs();
    QCOMPARE(m_model->rowCount(), 3);
    QCOMPARE(m_model->rowAt(0).sessionId, QStringLiteral("sA"));
    QCOMPARE(m_model->rowAt(1).sessionId, QStringLiteral("sB"));
    QCOMPARE(m_model->rowAt(2).sessionId, QStringLiteral("sC"));
    m_model->sessionRef(2);
    QVERIFY(!m_model->rowAt(0).isLoaded());
    QVERIFY(!m_model->rowAt(1).isLoaded());
    QVERIFY(m_model->rowAt(2).isLoaded());
}

QString ColumnCacheTest::descriptionOf(const QString &sessionId) const
{
    const int row = m_model->getSessionRow(sessionId);
    if (row < 0)
        return QStringLiteral("<no row>");
    const SessionRow &sr = std::as_const(*m_model).rowAt(row);
    return sr.isLoaded() ? sr.session->getAttribute("_DESCRIPTION").toString()
                         : sr.cachedValues.value(kD).toString();
}

bool ColumnCacheTest::fileHasDescription(const QString &sessionId, const char *text) const
{
    return readFileBytes(sessionFilePath(sessionId))
        .contains(QByteArray("$VAR,_DESCRIPTION,") + text + QByteArray("\n"));
}

void ColumnCacheTest::bulkEditFollowsSessionsAcrossSort()
{
    startWithMixedRows();
    if (QTest::currentTestFailed())
        return;

    m_model->startBulkEdit({0, 2}, kD, QStringLiteral("bulk"));
    // Queuing the edit removed the cached description of the stub sA, and a
    // row without a value sorts last
    m_model->sort(kD, Qt::AscendingOrder);
    QCOMPARE(m_model->rowAt(0).sessionId, QStringLiteral("sB"));
    QCOMPARE(m_model->rowAt(1).sessionId, QStringLiteral("sC"));
    QCOMPARE(m_model->rowAt(2).sessionId, QStringLiteral("sA"));

    QVERIFY(waitForIdle(*m_model));

    QCOMPARE(descriptionOf("sA"), QStringLiteral("bulk"));
    QCOMPARE(descriptionOf("sB"), QStringLiteral("a"));
    QCOMPARE(descriptionOf("sC"), QStringLiteral("bulk"));
    QVERIFY(fileHasDescription("sA", "bulk"));
    QVERIFY(fileHasDescription("sB", "a"));
    QVERIFY(fileHasDescription("sC", "bulk"));
    QVERIFY(!m_model->rowAt(m_model->getSessionRow("sA")).isLoaded());
    QVERIFY(!m_model->rowAt(m_model->getSessionRow("sC")).dirty);
}

void ColumnCacheTest::bulkEditSkipsRemovedSession()
{
    startWithMixedRows();
    if (QTest::currentTestFailed())
        return;

    m_model->startBulkEdit({0, 2}, kD, QStringLiteral("bulk"));
    QVERIFY(m_model->removeSessions({QStringLiteral("sA")}));
    QCOMPARE(m_model->rowCount(), 2);

    BulkEditSignals bulk(*m_model);
    QSignalSpy modelChangedSpy(m_model.get(), &SessionModel::modelChanged);
    {
        WarningCapture warnings;
        QVERIFY(waitForIdle(*m_model));
        QCOMPARE(warnings.count(), 0);
    }

    QCOMPARE(descriptionOf("sB"), QStringLiteral("a"));
    QCOMPARE(descriptionOf("sC"), QStringLiteral("bulk"));
    QVERIFY(fileHasDescription("sB", "a"));
    QVERIFY(fileHasDescription("sC", "bulk"));

    // The skipped item counts as done; the batch finishes once
    QCOMPARE(bulk.activations(), 1);
    QCOMPARE(bulk.lastProgress(), qMakePair(0, 2));
    QCOMPARE(modelChangedSpy.count(), 1);      // finishBulkEdit

    // Nothing is left over for a later batch
    BulkEditSignals second(*m_model);
    m_model->startBulkEdit({0}, kD, QStringLiteral("again"));
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(second.lastProgress(), qMakePair(0, 1));
    QCOMPARE(descriptionOf("sB"), QStringLiteral("again"));
    QCOMPARE(descriptionOf("sC"), QStringLiteral("bulk"));
}

// A merge appends the session it creates, so the rows are sorted as well: the
// new session takes row 0, which the batch's index for sA named.
void ColumnCacheTest::bulkEditFollowsSessionsAcrossMerge()
{
    startWithMixedRows();
    if (QTest::currentTestFailed())
        return;

    m_model->startBulkEdit({0, 2}, kD, QStringLiteral("bulk"));
    const QList<MergeResult> results = m_model->mergeSessions({describedSession("sN", "0")});
    QCOMPARE(results.size(), 1);
    QCOMPARE(results.first().outcome, MergeResult::Outcome::Created);
    m_model->sort(kD, Qt::AscendingOrder);
    QCOMPARE(m_model->rowCount(), 4);
    QCOMPARE(m_model->rowAt(0).sessionId, QStringLiteral("sN"));
    QCOMPARE(m_model->rowAt(1).sessionId, QStringLiteral("sB"));
    QCOMPARE(m_model->rowAt(2).sessionId, QStringLiteral("sC"));
    QCOMPARE(m_model->rowAt(3).sessionId, QStringLiteral("sA"));    // no cached description: last

    QVERIFY(waitForIdle(*m_model));

    QCOMPARE(descriptionOf("sN"), QStringLiteral("0"));
    QCOMPARE(descriptionOf("sA"), QStringLiteral("bulk"));
    QCOMPARE(descriptionOf("sB"), QStringLiteral("a"));
    QCOMPARE(descriptionOf("sC"), QStringLiteral("bulk"));
    QVERIFY(fileHasDescription("sN", "0"));
    QVERIFY(fileHasDescription("sA", "bulk"));
    QVERIFY(fileHasDescription("sB", "a"));
    QVERIFY(fileHasDescription("sC", "bulk"));
}

// A row known by its file stem gets its real SESSION_ID while the edit is
// queued; the queued item follows the row.
void ColumnCacheTest::bulkEditFollowsIdentityStubRemap()
{
    TestEnvironment &env = TestEnvironment::instance();
    LogbookManager &logbook = LogbookManager::instance();

    startWithLoadedSessions({gyroSession()});
    QCOMPARE(sessionCsvFiles().size(), 1);
    const QString stem = QFileInfo(sessionCsvFiles().first()).completeBaseName();
    const QString csvPath = env.sessionsDir() + QLatin1Char('/') + stem + QStringLiteral(".csv");
    QVERIFY(stem != QStringLiteral("g1"));
    m_model.reset();

    QVERIFY(QFile::remove(env.indexPath()));
    env.reopenLogbook();
    logbook.initialize();
    QVERIFY(logbook.hasDeferredScan());

    m_model = std::make_unique<SessionModel>();
    m_model->populateFromUuids(logbook.scannedUuids());
    QCOMPARE(m_model->rowCount(), 1);
    QCOMPARE(m_model->rowAt(0).sessionId, stem);

    BulkEditSignals bulk(*m_model);
    m_model->startBulkEdit({0}, kD, QStringLiteral("bulk"));
    m_model->resolveIdentityStubs();
    QCOMPARE(m_model->rowAt(0).sessionId, QStringLiteral("g1"));

    QVERIFY(waitForIdle(*m_model));

    QCOMPARE(bulk.lastProgress(), qMakePair(0, 1));
    QCOMPARE(m_model->rowAt(0).sessionId, QStringLiteral("g1"));
    QCOMPARE(descriptionOf("g1"), QStringLiteral("bulk"));
    QVERIFY(readFileBytes(csvPath).contains("$VAR,_DESCRIPTION,bulk\n"));
    QCOMPARE(sessionCsvFiles(), QStringList({stem + QStringLiteral(".csv")}));
}

// The edited column is disabled while the edit is queued: the attribute is
// the truth and is edited all the same; there is no column left to refresh.
void ColumnCacheTest::bulkEditSurvivesDisabledColumn()
{
    startWithMixedRows();
    if (QTest::currentTestFailed())
        return;

    m_model->startBulkEdit({0, 2}, kD, QStringLiteral("bulk"));
    LogbookColumnStore::instance().setColumns({m_g, m_e});      // restored in cleanup()
    QCOMPARE(m_model->columnCount(), 2);

    QVERIFY(waitForIdle(*m_model));

    QVERIFY(fileHasDescription("sA", "bulk"));
    QVERIFY(fileHasDescription("sB", "a"));
    QVERIFY(fileHasDescription("sC", "bulk"));
    QCOMPARE(m_model->sessionRef(m_model->getSessionRow("sA")).getAttribute("_DESCRIPTION").toString(),
             QStringLiteral("bulk"));
    QCOMPARE(m_model->sessionRef(m_model->getSessionRow("sB")).getAttribute("_DESCRIPTION").toString(),
             QStringLiteral("a"));
    QCOMPARE(m_model->sessionRef(m_model->getSessionRow("sC")).getAttribute("_DESCRIPTION").toString(),
             QStringLiteral("bulk"));
}

// An interrupted save cannot leave cached columns that disagree
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

    m_altitudes = std::make_unique<AltitudeMarkerManager>();
    PreferencesManager::instance().setValue(PreferenceKeys::AltitudeMarkersUnits, QStringLiteral("Metric"));
    m_altitudes->refresh();     // nothing configured: registers nothing
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

// Acceptance 13, for rows that are not loaded: when a registration is REMOVED,
// the cached column values it produced are discarded in every row - a loaded
// one and a stub alike - without touching a session file.
void ColumnCacheTest::altitudeMarkerRemovalDiscardsStubValues()
{
    constexpr int kA = 3;
    LogbookColumn altitudeColumn;
    altitudeColumn.type = ColumnType::SessionAttribute;
    altitudeColumn.attributeKey = QStringLiteral("_ALTITUDE_1000_M");
    LogbookColumnStore::instance().setColumns({m_d, m_g, m_e, altitudeColumn});     // restored in cleanup()

    LogbookManager &logbook = LogbookManager::instance();
    startWithLoadedSessions({DescentFixture::load("s1"), DescentFixture::load("s2")});
    restartAsStubs();
    const int row1 = m_model->getSessionRow("s1");
    const int row2 = m_model->getSessionRow("s2");
    m_model->sessionRef(row1);      // s1 loaded, s2 stays a stub
    QVERIFY(m_model->rowAt(row1).isLoaded());
    QVERIFY(!m_model->rowAt(row2).isLoaded());

    m_altitudes = std::make_unique<AltitudeMarkerManager>();
    writeAltitudes({1000});
    PreferencesManager::instance().setValue(PreferenceKeys::AltitudeMarkersUnits, QStringLiteral("Metric"));
    m_altitudes->refresh();
    m_model->flushPendingInvalidations();
    QVERIFY(waitForIdle(*m_model));

    const DependencyKey altitudeName = DependencyKey::attribute(QStringLiteral("_ALTITUDE_1000_M"));
    QVERIFY(CalculationRegistry::instance().hasCandidateFor(altitudeName));
    QVERIFY(isNear(cached(row1, kA).toDouble(), T0 + 71.5));
    QVERIFY(isNear(cached(row2, kA).toDouble(), T0 + 71.5));
    QVERIFY(!m_model->rowAt(row2).isLoaded());
    logbook.flushIndex();
    QVERIFY(indexValue(readIndex(), "s1", altitudeColumn).isDouble());
    QVERIFY(indexValue(readIndex(), "s2", altitudeColumn).isDouble());

    const QByteArray csv1 = readFileBytes(sessionFilePath("s1"));
    const QByteArray csv2 = readFileBytes(sessionFilePath("s2"));
    const QString withAltitude = logbook.cacheEnvironment();
    QCOMPARE(withAltitude, calculationEnvironmentFingerprint());

    writeAltitudes({});             // the manager refreshes and unregisters
    m_model->flushPendingInvalidations();
    QVERIFY(waitForIdle(*m_model));

    QVERIFY(!CalculationRegistry::instance().hasCandidateFor(altitudeName));
    QVERIFY(!cached(row1, kA).isValid());
    QVERIFY(!cached(row2, kA).isValid());

    logbook.flushIndex();
    QVERIFY(!indexValue(readIndex(), "s1", altitudeColumn).isDouble());
    QVERIFY(!indexValue(readIndex(), "s2", altitudeColumn).isDouble());

    QVERIFY(!m_model->rowAt(row1).dirty);
    QVERIFY(!m_model->rowAt(row2).dirty);
    QCOMPARE(readFileBytes(sessionFilePath("s1")), csv1);
    QCOMPARE(readFileBytes(sessionFilePath("s2")), csv2);

    QVERIFY(logbook.cacheEnvironment() != withAltitude);
    QCOMPARE(logbook.cacheEnvironment(), calculationEnvironmentFingerprint());

    // The other columns came back
    QCOMPARE(cached(row2, kE).toDouble(), T0 + 9.0);
}

// A failed save is reported once with the exporter's reason and is not retried
// by the idle saver (the scheduler goes idle). The previous file is intact, the
// row STAYS DIRTY (flagged saveFailed) because its in-memory state is the only
// copy of the edit, and the affected values stay out of the index. Once the
// cause is gone, a flush (what shutdown does) saves it and clears the flag.
void ColumnCacheTest::saveFailureIsReported()
{
    startWithLoadedSessions({gyroSession()});
    const QString csvPath = sessionFilePath("g1");
    const QByteArray csvBytes = readFileBytes(csvPath);

    // Make the loaded session ragged behind the model's back: the exporter
    // refuses it, which fails the save without any file-system trick.
    m_model->sessionRef(0).setMeasurement("IMU", "wx", {1.0, 2.0});

    {
        WarningCapture warnings;
        QVERIFY(m_model->updateAttribute("g1", "_DESCRIPTION", QStringLiteral("unsaved")));
        QVERIFY(waitForIdle(*m_model));     // goes idle: no retry loop

        const QStringList reported = warnings.matching(QStringLiteral("was not saved"));
        QCOMPARE(reported.size(), 1);
        QVERIFY2(reported.first().contains(QStringLiteral("g1")), qPrintable(reported.first()));
        QVERIFY2(reported.first().contains(
                     QStringLiteral("Sensor 'IMU' has columns of unequal length (time: 3, wx: 2)")),
                 qPrintable(reported.first()));

        // Still idle and still exactly one report after more event-loop time
        QTest::qWait(50);
        QVERIFY(waitForIdle(*m_model));
        QCOMPARE(warnings.matching(QStringLiteral("was not saved")).size(), 1);
    }

    QVERIFY(m_model->rowAt(0).dirty);
    QVERIFY(m_model->rowAt(0).saveFailed);
    QVERIFY(m_model->rowAt(0).isLoaded());
    QCOMPARE(m_model->sessionRef(0).storedAttribute("_DESCRIPTION"), QVariant(QStringLiteral("unsaved")));
    QCOMPARE(readFileBytes(csvPath), csvBytes);

    // index.json still describes the file on disk: no value for the edited
    // column, although the new value is cached in memory for display
    QVERIFY(LogbookManager::instance().hasUnsavedColumns("g1"));
    QCOMPARE(cached(0, kD).toString(), QStringLiteral("unsaved"));
    LogbookManager::instance().flushIndex();
    const QJsonObject root = readIndex();
    QVERIFY(indexValue(root, "g1", m_d).isUndefined());
    QVERIFY(isNear(indexValue(root, "g1", m_g).toDouble(), 1.72032));

    // A flush while the cause persists fails again: reported once more, and
    // the row is still dirty.
    {
        WarningCapture warnings;
        m_model->flushDirtySessions();
        QCOMPARE(warnings.matching(QStringLiteral("was not saved")).size(), 1);
    }
    QVERIFY(m_model->rowAt(0).dirty);
    QVERIFY(m_model->rowAt(0).saveFailed);
    QCOMPARE(readFileBytes(csvPath), csvBytes);
    QVERIFY(indexValue(readIndex(), "g1", m_d).isUndefined());

    // The cause is removed; the flush at shutdown saves the edit.
    m_model->sessionRef(0).setMeasurement("IMU", "wx", {1.0, 2.0, 3.0});
    {
        WarningCapture warnings;
        m_model->flushDirtySessions();
        QCOMPARE(warnings.count(), 0);
    }
    QVERIFY(!m_model->rowAt(0).dirty);
    QVERIFY(!m_model->rowAt(0).saveFailed);
    QVERIFY(!LogbookManager::instance().hasUnsavedColumns("g1"));
    QVERIFY(readFileBytes(csvPath).contains("$VAR,_DESCRIPTION,unsaved\n"));
    QCOMPARE(indexValue(readIndex(), "g1", m_d).toString(), QStringLiteral("unsaved"));
}

// The in-memory session of a row whose save failed is the only copy of the
// edit: the LRU passes over it, whatever the capacity, and evicts the others.
void ColumnCacheTest::failedSaveRowIsNotEvicted()
{
    startWithLoadedSessions({gyroSession("g1"), gyroSession("g2"), gyroSession("g3")});
    const int row1 = m_model->getSessionRow("g1");
    const int row2 = m_model->getSessionRow("g2");
    const int row3 = m_model->getSessionRow("g3");
    const QByteArray csvBytes = readFileBytes(sessionFilePath("g1"));

    m_model->sessionRef(row1).setMeasurement("IMU", "wx", {1.0, 2.0});
    m_model->sessionRef(row2);      // g1 is now the least recently used
    m_model->sessionRef(row3);

    WarningCapture warnings;
    QVERIFY(m_model->updateAttribute("g1", "_DESCRIPTION", QStringLiteral("only copy")));
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(m_model->rowAt(row1).saveFailed);
    m_model->sessionRef(row2);
    m_model->sessionRef(row3);

    // Room for one session. g1 is the least recently used row but cannot go;
    // the evictable rows are evicted until the capacity is met, and g1 stays.
    const auto restoreCapacity = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    });
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 1);
    QVERIFY(m_model->rowAt(row1).isLoaded());
    QVERIFY(m_model->rowAt(row1).dirty);
    QVERIFY(m_model->rowAt(row1).saveFailed);
    QVERIFY(!m_model->rowAt(row2).isLoaded());
    QVERIFY(!m_model->rowAt(row3).isLoaded());
    QCOMPARE(m_model->sessionRef(row1).storedAttribute("_DESCRIPTION"), QVariant(QStringLiteral("only copy")));

    // Not saved behind our back, and not reported again by the eviction pass.
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(warnings.matching(QStringLiteral("was not saved")).size(), 1);
    QCOMPARE(readFileBytes(sessionFilePath("g1")), csvBytes);
    QVERIFY(indexValue(readIndex(), "g1", m_d).isUndefined());

    // Once it can be saved it is an ordinary row again: saved by the flush,
    // and evictable.
    m_model->sessionRef(row1).setMeasurement("IMU", "wx", {1.0, 2.0, 3.0});
    m_model->flushDirtySessions();
    QVERIFY(!m_model->rowAt(row1).dirty);
    QVERIFY(!m_model->rowAt(row1).saveFailed);
    QVERIFY(readFileBytes(sessionFilePath("g1")).contains("$VAR,_DESCRIPTION,only copy\n"));
    m_model->sessionRef(row2);      // loading another row evicts g1 (capacity 1)
    QVERIFY(!m_model->rowAt(row1).isLoaded());
    QCOMPARE(cached(row1, kD).toString(), QStringLiteral("only copy"));
}

// A new edit to a row whose save failed queues it for the idle saver again;
// a save that then succeeds clears the flag. The bulk edit follows the same
// contract as a single edit.
void ColumnCacheTest::newEditRetriesFailedSave()
{
    startWithLoadedSessions({gyroSession()});
    const QString csvPath = sessionFilePath("g1");
    m_model->sessionRef(0).setMeasurement("IMU", "wx", {1.0, 2.0});

    {
        WarningCapture warnings;
        m_model->startBulkEdit({0}, kD, QStringLiteral("bulk"));
        QVERIFY(waitForIdle(*m_model));
        QCOMPARE(warnings.matching(QStringLiteral("was not saved")).size(), 1);
    }
    QVERIFY(m_model->rowAt(0).dirty);
    QVERIFY(m_model->rowAt(0).saveFailed);
    QVERIFY(readFileBytes(csvPath).contains("$VAR,_DESCRIPTION,first\n"));
    QVERIFY(indexValue(readIndex(), "g1", m_d).isUndefined());

    // Another edit while the cause persists: one more attempt, one more report.
    {
        WarningCapture warnings;
        QVERIFY(m_model->updateAttribute("g1", "_DESCRIPTION", QStringLiteral("second try")));
        QVERIFY(!m_model->rowAt(0).saveFailed);     // queued again
        QVERIFY(waitForIdle(*m_model));
        QCOMPARE(warnings.matching(QStringLiteral("was not saved")).size(), 1);
    }
    QVERIFY(m_model->rowAt(0).dirty);
    QVERIFY(m_model->rowAt(0).saveFailed);

    // The cause is removed and the row is edited again: the idle saver saves it.
    m_model->sessionRef(0).setMeasurement("IMU", "wx", {1.0, 2.0, 3.0});
    {
        WarningCapture warnings;
        QVERIFY(m_model->updateAttribute("g1", "_DESCRIPTION", QStringLiteral("third try")));
        QVERIFY(waitForIdle(*m_model));
        QCOMPARE(warnings.count(), 0);
    }
    QVERIFY(!m_model->rowAt(0).dirty);
    QVERIFY(!m_model->rowAt(0).saveFailed);
    QVERIFY(readFileBytes(csvPath).contains("$VAR,_DESCRIPTION,third try\n"));
    QCOMPARE(indexValue(readIndex(), "g1", m_d).toString(), QStringLiteral("third try"));
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

// Columns with one definition collapse into the first of them; everything
// else keeps its place.
void ColumnCacheTest::uniqueColumnsKeepsFirstOccurrence()
{
    LogbookColumn delta;
    delta.type = ColumnType::Delta;
    delta.sensorID = QStringLiteral("IMU");
    delta.measurementID = QStringLiteral("wx");
    delta.measurementType = QStringLiteral("rotation");
    delta.markerAttributeKey = QStringLiteral("_M");
    delta.marker2AttributeKey = QStringLiteral("_N");

    LogbookColumn deltaReversed = delta;
    deltaReversed.markerAttributeKey = QStringLiteral("_N");
    deltaReversed.marker2AttributeKey = QStringLiteral("_M");

    // Nothing to collapse: returned as given. m_g and delta share the
    // measurement and first marker and differ in type.
    const QVector<LogbookColumn> distinct = {m_g, m_d, delta, m_e, deltaReversed};
    QCOMPARE(uniqueLogbookColumns(distinct), distinct);
    QCOMPARE(uniqueLogbookColumns({}), QVector<LogbookColumn>());

    // One duplicate of each column type; display-only fields do not make a
    // column a different one.
    LogbookColumn dRelabelled = m_d;
    dRelabelled.customLabel = QStringLiteral("Notes");
    LogbookColumn gDisabled = m_g;
    gDisabled.enabled = false;

    QVector<LogbookColumn> unique = uniqueLogbookColumns({m_d, m_g, dRelabelled, delta, m_e, gDisabled, delta, m_d});
    QCOMPARE(unique, (QVector<LogbookColumn>{m_d, m_g, delta, m_e}));
    QVERIFY(unique[0].customLabel.isEmpty());       // the first occurrence's label
    QVERIFY(unique[1].enabled);

    unique = uniqueLogbookColumns({dRelabelled, m_d});
    QCOMPARE(unique.size(), 1);
    QCOMPARE(unique[0].customLabel, QStringLiteral("Notes"));

    // A hidden first occurrence takes over the visibility of a shown duplicate
    unique = uniqueLogbookColumns({gDisabled, m_d, m_g});
    QCOMPARE(unique, (QVector<LogbookColumn>{m_g, m_d}));

    QCOMPARE(logbookColumnDefinitionKey(m_d), logbookColumnDefinitionKey(dRelabelled));
    QVERIFY(logbookColumnDefinitionKey(delta) != logbookColumnDefinitionKey(deltaReversed));
    QVERIFY(logbookColumnDefinitionKey(delta) != logbookColumnDefinitionKey(m_g));
}

// Settings that hold the same column twice are cleaned when they are read,
// and the cleaned list is written back.
void ColumnCacheTest::storeCollapsesDuplicatesOnLoad()
{
    const auto storedAttributeKeys = []() {
        QStringList keys;
        QSettings settings;
        const int count = settings.beginReadArray(QStringLiteral("logbook/columns"));
        for (int i = 0; i < count; ++i) {
            settings.setArrayIndex(i);
            keys.append(settings.value(QStringLiteral("attributeKey")).toString());
        }
        settings.endArray();
        return keys;
    };

    {
        const QStringList keys = {m_e.attributeKey, m_d.attributeKey, m_e.attributeKey};
        QSettings settings;
        settings.remove(QStringLiteral("logbook/columns"));
        settings.beginWriteArray(QStringLiteral("logbook/columns"), int(keys.size()));
        for (int i = 0; i < keys.size(); ++i) {
            settings.setArrayIndex(i);
            settings.setValue(QStringLiteral("type"), int(ColumnType::SessionAttribute));
            settings.setValue(QStringLiteral("attributeKey"), keys[i]);
            settings.setValue(QStringLiteral("enabled"), true);
        }
        settings.endArray();
    }
    QCOMPARE(storedAttributeKeys().size(), 3);

    LogbookColumnStore &store = LogbookColumnStore::instance();
    store.load();
    QCOMPARE(store.columns(), (QVector<LogbookColumn>{m_e, m_d}));
    QCOMPARE(storedAttributeKeys(), (QStringList{m_e.attributeKey, m_d.attributeKey}));

    // The setter collapses as well
    store.setColumns({m_d, m_g, m_d, m_e, m_g});
    QCOMPARE(store.columns(), (QVector<LogbookColumn>{m_d, m_g, m_e}));
    QCOMPARE(storedAttributeKeys().size(), 3);
}

// The index stores one value per column definition. Live columns that share a
// definition each receive it, so a row never comes back with fewer values than
// there are columns just because two columns are the same.
void ColumnCacheTest::indexValueReachesEveryColumnOfItsDefinition()
{
    startWithLoadedSessions({gyroSession()});

    LogbookManager &logbook = LogbookManager::instance();
    m_model.reset();
    TestEnvironment::instance().reopenLogbook();
    logbook.initialize();

    const QMap<int, QVariant> values = logbook.cachedColumnValues({m_d, m_g, m_e, m_d, m_g}).value(QStringLiteral("g1"));
    QCOMPARE(values.size(), 5);
    QCOMPARE(values.value(0).toString(), QStringLiteral("first"));
    QCOMPARE(values.value(3).toString(), QStringLiteral("first"));
    QVERIFY(isNear(values.value(1).toDouble(), 1.72032));
    QVERIFY(isNear(values.value(4).toDouble(), 1.72032));
}

// The same column enabled twice must not make every start reload every
// session: after one full column pass and a restart there is nothing to do.
void ColumnCacheTest::duplicateColumnCausesNoWorkAfterRestart()
{
    LogbookColumnStore::instance().setColumns({m_d, m_g, m_e, m_d});    // restored in cleanup()

    startWithLoadedSessions({gyroSession(QStringLiteral("g1")), gyroSession(QStringLiteral("g2"))});

    // First start: every row is a stub and whatever is missing gets computed
    restartAsStubs();
    m_model->startColumnWorker();
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(!LogbookManager::instance().indexNeedsFlush());

    // Second start: the index has it all
    restartAsStubs();
    const int columnCount = m_model->columnCount();
    for (int row = 0; row < m_model->rowCount(); ++row)
        QCOMPARE(m_model->rowAt(row).cachedValues.size(), columnCount);

    const QByteArray indexBefore = readFileBytes(TestEnvironment::instance().indexPath());
    m_model->resetColumnWorkStats();
    m_model->startColumnWorker();
    QVERIFY(waitForIdle(*m_model));

    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 0);
    QCOMPARE(m_model->columnWorkStats().valuesComputed, 0);
    QVERIFY(!m_model->rowAt(0).isLoaded());
    QVERIFY(!m_model->rowAt(1).isLoaded());
    QCOMPARE(readFileBytes(TestEnvironment::instance().indexPath()), indexBefore);
}

FLYSIGHT_TEST_MAIN(ColumnCacheTest)
#include "tst_column_cache.moc"
