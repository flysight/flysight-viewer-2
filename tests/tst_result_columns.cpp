// Logbook columns over explicit results on a real SessionModel, a real logbook
// and a real JobQueue, with fast synthetic explicit calculations
// (store-requested-calculations):
//
//  - a loaded row caches the engine's value: unavailable before any request,
//    the restored or published value after one; index.json stamps it with the
//    session's record set ("records": calculation id -> result version);
//  - writing or deleting a record drops the values over it at once (only
//    those), and the next event-loop pass computes them again;
//  - an unloaded row without a record is settled as unavailable with no
//    load; with one, the column worker restores the session's records into
//    its temporary copy (the checks of a load: a stale record deleted, an
//    unreadable one skipped and its values PENDING) and caches the value
//    computed from it - the row is never loaded, no record is written,
//    nothing is run; a copy whose missing columns are all on demand reads no
//    record;
//  - crash points between a record write / delete and the index flush, an
//    index written by an older build (no stamp), a result-version change, a
//    failed write, a session not saved yet, environment changes (which
//    discard the cached values of the columns whose closure they reach, and
//    only those, but never make a record stale), and a record skipped at a
//    load (its values never cached while skipped);
//  - with column demand (a CalculationDemand), the column worker settles and
//    computes exactly as without it, and a stale record it deletes moves the
//    pair into demand.
//
// Calculations (registered once, before any initialize(); literals below):
//
// | Id             | Inputs           | Output                                 | resultVersion |
// |----------------|------------------|----------------------------------------|---------------|
// | test.columns.x | attr _DESCRIPTION| X_OUT = "x:" + _DESCRIPTION (QString)  | ""            |
// | test.columns.y | attr Y_IN        | Y_OUT = Y_IN * 2 (double)              | "y-v1"        |
//
// Sessions s1 (_DESCRIPTION "d1", Y_IN 3) and s2 ("d2", 5): X gives "x:d1",
// Y gives 6 on s1. Columns {_DESCRIPTION, X_OUT, Y_OUT} = kD, kX, kY.
//
// Expected values are literals, never recomputed with the code under test.
// Only the skipped-record rows use a platform mechanism (a lock on Windows,
// permission bits elsewhere) and skip where it is not honoured. Nothing
// depends on case sensitivity or directory iteration order.

#include <functional>
#include <memory>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QtTest>

#include "calculationdemand.h"
#include "calculationrecord.h"
#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "engine/storedcalculationresult.h"
#include "jobfixture.h"
#include "jobmodel.h"
#include "jobqueue.h"
#include "logbookcolumn.h"
#include "logbookmanager.h"
#include "logbookprobe.h"
#include "plotfixture.h"
#include "plotmodel.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "sessiondata.h"
#include "sessionmodel.h"
#include "testenvironment.h"
#include "testmain.h"
#include "testutil.h"

using namespace FlySight;
using namespace FlySightTest;

Q_DECLARE_METATYPE(FlySight::DependencyKey)

namespace {

const QString kCalcX = QStringLiteral("test.columns.x");
const QString kCalcY = QStringLiteral("test.columns.y");
const QString kExtra = QStringLiteral("test.columns.extra");
const QString kShadow = QStringLiteral("test.columns.shadow");
const QString kEncodedX = QStringLiteral("test%2Ecolumns%2Ex");
const QString kEncodedY = QStringLiteral("test%2Ecolumns%2Ey");

constexpr int kD = 0;   // column indices
constexpr int kX = 1;
constexpr int kY = 2;

LogbookColumn attributeColumn(const char *key)
{
    LogbookColumn col;
    col.type = ColumnType::SessionAttribute;
    col.attributeKey = QString::fromLatin1(key);
    return col;
}

LogbookColumn xColumn() { return attributeColumn("X_OUT"); }
LogbookColumn yColumn() { return attributeColumn("Y_OUT"); }

/// tst_column_cache's gyro session shape (loads from its file), with the
/// inputs of the two calculations.
SessionData columnSession(const QString &id, const QString &description, double yIn)
{
    constexpr double T0 = 1704110400.0;     // 2024-01-01T12:00:00Z
    SessionData s;
    s.setAttribute("SESSION_ID", id);
    s.setAttribute("DEVICE_ID", QStringLiteral("test-device"));
    s.setAttribute("_DESCRIPTION", description);
    s.setAttribute("Y_IN", yIn);
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

/// The bytes of a file; a null QByteArray when it does not exist or is not a file.
QByteArray bytesOf(const QString &path)
{
    QFile file(path);
    if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

/// The session entry of index.json without the value of one column.
void removeIndexValue(QJsonObject &root, const QString &sessionId, const LogbookColumn &col)
{
    const QString columnId = indexColumnId(root, col);
    QJsonObject sessions = root[QStringLiteral("sessions")].toObject();
    QJsonObject entry = sessions[sessionId].toObject();
    QJsonObject values = entry[QStringLiteral("values")].toObject();
    values.remove(columnId);
    entry[QStringLiteral("values")] = values;
    sessions[sessionId] = entry;
    root[QStringLiteral("sessions")] = sessions;
}

void setIndexValue(QJsonObject &root, const QString &sessionId, const LogbookColumn &col, const QJsonValue &value)
{
    const QString columnId = indexColumnId(root, col);
    QJsonObject sessions = root[QStringLiteral("sessions")].toObject();
    QJsonObject entry = sessions[sessionId].toObject();
    QJsonObject values = entry[QStringLiteral("values")].toObject();
    values[columnId] = value;
    entry[QStringLiteral("values")] = values;
    sessions[sessionId] = entry;
    root[QStringLiteral("sessions")] = sessions;
}

void setIndexStamp(QJsonObject &root, const QString &sessionId, const QJsonObject &stamp)
{
    QJsonObject sessions = root[QStringLiteral("sessions")].toObject();
    QJsonObject entry = sessions[sessionId].toObject();
    entry[QStringLiteral("records")] = stamp;
    sessions[sessionId] = entry;
    root[QStringLiteral("sessions")] = sessions;
}

QJsonValue stampOf(std::initializer_list<std::pair<QString, QString>> records)
{
    QJsonObject stamp;
    for (const auto &record : records)
        stamp[record.first] = record.second;
    return stamp;
}

bool allEntriesHaveStamp(const QJsonObject &root)
{
    const QJsonObject sessions = root[QStringLiteral("sessions")].toObject();
    for (auto it = sessions.constBegin(); it != sessions.constEnd(); ++it) {
        if (!it.value().toObject().value(QStringLiteral("records")).isObject())
            return false;
    }
    return !sessions.isEmpty();
}

} // namespace

class ResultColumnsTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    void columnExplicitCalculations();
    void unrequestedIsCachedUnavailable();
    void stampWrittenOnFlush();
    void publishedResultIsCached_data();
    void publishedResultIsCached();
    void restartShowsCachedValueWithoutLoading();
    void noRecordStaysUnavailableAfterRestart();
    void inputChangeDropsCachedValue();
    void onlyDependentColumnsDrop();
    void staleRecordOnLoadDropsCachedValue();
    void workerRestoresStoredResult();
    void workerSkipsUnreadableRecord_data();
    void workerSkipsUnreadableRecord();
    void onDemandColumnsReadNoRecord();
    void crashAfterRecordWrite();
    void crashAfterRecordDelete();
    void rewriteAfterDropFlushesIndexFirst();
    void writeAfterStartupDropFlushesIndexFirst_data();
    void writeAfterStartupDropFlushesIndexFirst();
    void oldIndexWithoutStamp_data();
    void oldIndexWithoutStamp();
    void resultVersionChangeDropsCachedValue();
    void writeFailureKeepsValueOutOfIndex();
    void recordBeforeFirstSaveIsCachedAfterSave();
    void environmentChangeDiscardsReachedColumn();
    void registryChangeKeepsLoadedRowConfirmed();
    void skippedRecordValuesStayOutOfIndex_data();
    void skippedRecordValuesStayOutOfIndex();
    void managerDropsDependentValues();
    void deletingSessionRemovesStamp();
    void removingReservedSessionForgetsIt();
    void deletedCacheFolderForgetsRequests();
    void columnWorkerIsUnchangedByDemand_data();
    void columnWorkerIsUnchangedByDemand();
    void staleRecordDeletedByWorkerCreatesDemand();

private:
    SessionData &session(const QString &id) { return m_model->sessionRef(m_model->getSessionRow(id)); }
    CalculationEngine &engine(const QString &id) { return session(id).calculationEngine(); }
    int row(const QString &id) const { return m_model->getSessionRow(id); }
    const SessionRow &rowState(const QString &id) const { return std::as_const(*m_model).rowAt(row(id)); }
    bool isLoaded(const QString &id) const { return row(id) >= 0 && rowState(id).isLoaded(); }
    QVariant cached(const QString &id, int column) const { return rowState(id).cachedValues.value(column); }
    bool isCached(const QString &id, int column) const { return rowState(id).cachedValues.contains(column); }
    QSet<int> pending(const QString &id) const { return rowState(id).pendingColumns; }
    QString cell(const QString &id, int column) const
    {
        return m_model->data(m_model->index(row(id), column), Qt::DisplayRole).toString();
    }
    const CalculationResultStore::Stats &stats() const { return m_model->storedResultStats(); }

    /// cache/<stem of id>.<encoded>.fvresult; the stem comes from index.json on disk.
    static QString recordPath(const QString &id, const QString &encoded)
    {
        return TestEnvironment::instance().cacheDir() + QLatin1Char('/') + sessionFileStem(id)
            + QLatin1Char('.') + encoded + QStringLiteral(".fvresult");
    }

    /// Requests a calculation synchronously on a loaded row, then lets the model go idle.
    [[nodiscard]] bool fit(const QString &id, const QString &calculationId)
    {
        return engine(id).request(calculationId).status == ResultStatus::Ok && waitForIdle(*m_model);
    }
    /// Makes each row a stub (as tst_result_store): touches it, evicts with
    /// capacity 0, and sets the capacity back to 50. Empty when every row is a stub.
    [[nodiscard]] QString evict(const QStringList &ids);
    /// Queue, then model.
    void resetModel();
    /// A simulated application restart: new logbook state, a new model of
    /// stubs from the index, a new queue. Runs no event-loop pass.
    void restart();
    /// A crash right after the last action: restart() with no event-loop pass
    /// before it, so the model's destructor saves and flushes nothing and a
    /// queued refresh dies with the model.
    void crash() { restart(); }
    /// readIndex, mutate, writeIndex. Only while no model exists.
    [[nodiscard]] bool editIndex(const std::function<void(QJsonObject &)> &mutate)
    {
        QJsonObject root = readIndex();
        mutate(root);
        return writeIndex(root);
    }

    std::unique_ptr<SessionModel> m_model;
    std::unique_ptr<JobQueue> m_queue;
    QStringList m_registryBefore;
    QStringList m_registered;
};

void ResultColumnsTest::initTestCase()
{
    TestEnvironment::instance().registerBuiltIns();
    PreferencesManager::instance().registerPreference(PreferenceKeys::LogbookColumnsVersion, 0);
    qRegisterMetaType<DependencyKey>();

    // Before every initialize(), for the whole process (cleanupTestCase undoes them)
    CalculationRegistry &registry = CalculationRegistry::instance();

    CalculationDescriptor x;
    x.id = kCalcX;
    x.policy = EvaluationPolicy::Explicit;
    x.inputs = {CalcInput::attribute(QStringLiteral("_DESCRIPTION"))};
    x.outputs = {DependencyKey::attribute(QStringLiteral("X_OUT"))};
    x.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute(
            QStringLiteral("X_OUT"), QStringLiteral("x:") + ctx.attribute(QStringLiteral("_DESCRIPTION")).toString());
    };

    CalculationDescriptor y;
    y.id = kCalcY;
    y.policy = EvaluationPolicy::Explicit;
    y.resultVersion = QStringLiteral("y-v1");
    y.inputs = {CalcInput::attribute(QStringLiteral("Y_IN"))};
    y.outputs = {DependencyKey::attribute(QStringLiteral("Y_OUT"))};
    y.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute(QStringLiteral("Y_OUT"),
                                                ctx.attribute(QStringLiteral("Y_IN")).toDouble() * 2.0);
    };

    for (const CalculationDescriptor &d : {x, y}) {
        QVERIFY(registry.registerCalculation(d));
        m_registered.append(d.id);
    }

    LogbookColumnStore::instance().setColumns({descriptionColumn(), xColumn(), yColumn()});
}

void ResultColumnsTest::cleanupTestCase()
{
    for (const QString &id : std::as_const(m_registered))
        CalculationRegistry::instance().unregister(id, CalculationRegistry::Removal::Change);
}

// Two loaded, hidden, unfocused, saved and indexed sessions.
void ResultColumnsTest::init()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();
    env.resetPreferencesToDefaults();
    LogbookManager::instance().initialize();
    m_registryBefore = CalculationRegistry::instance().registeredIds();

    m_model = std::make_unique<SessionModel>();
    m_model->mergeSessions({columnSession(QStringLiteral("s1"), QStringLiteral("d1"), 3.0),
                            columnSession(QStringLiteral("s2"), QStringLiteral("d2"), 5.0)});
    QCOMPARE(m_model->rowCount(), 2);
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(!sessionFileStem("s1").isEmpty());

    m_queue = std::make_unique<JobQueue>(m_model.get());
}

// Tear everything down, and only then check (see tst_jobqueue).
void ResultColumnsTest::cleanup()
{
    resetModel();
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    LogbookColumnStore::instance().setColumns({descriptionColumn(), xColumn(), yColumn()});

    QCOMPARE(CalculationRegistry::instance().registeredIds(), m_registryBefore);
    QCOMPARE(CalculationRegistry::instance().enrolledEngineCount(), 0);
}

QString ResultColumnsTest::evict(const QStringList &ids)
{
    for (const QString &id : ids) {
        if (row(id) < 0)
            return id + QStringLiteral(" has no row");
        session(id);
    }
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 0);
    QString error;
    for (const QString &id : ids) {
        if (isLoaded(id) && error.isEmpty())
            error = id + QStringLiteral(" is still loaded");
    }
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    return error;
}

void ResultColumnsTest::resetModel()
{
    if (m_queue)
        m_queue->shutdown();
    m_queue.reset();
    m_model.reset();
}

void ResultColumnsTest::restart()
{
    resetModel();

    LogbookManager &logbook = LogbookManager::instance();
    TestEnvironment::instance().reopenLogbook();
    logbook.initialize();

    m_model = std::make_unique<SessionModel>();
    m_model->populateFromIndex(logbook.cachedColumnValues(LogbookColumnStore::instance().enabledColumns()),
                               logbook.lastAccessedMap());
    m_queue = std::make_unique<JobQueue>(m_model.get());
}

// ---- The column's calculations ----------------------------------------------------------

void ResultColumnsTest::columnExplicitCalculations()
{
    const CalculationRegistry &registry = CalculationRegistry::instance();
    QCOMPARE(logbookColumnExplicitCalculations(descriptionColumn(), registry), QStringList());
    QCOMPARE(logbookColumnExplicitCalculations(gyroColumn(), registry), QStringList());
    QCOMPARE(logbookColumnExplicitCalculations(xColumn(), registry), QStringList({kCalcX}));
    QCOMPARE(logbookColumnExplicitCalculations(yColumn(), registry), QStringList({kCalcY}));
}

// ---- Loaded rows ----------------------------------------------------------------------

void ResultColumnsTest::unrequestedIsCachedUnavailable()
{
    const QJsonObject root = readIndex();
    for (const char *id : {"s1", "s2"}) {
        QVERIFY(isCached(id, kX));
        QVERIFY(!cached(id, kX).isValid());
        QVERIFY(isCached(id, kY));
        QVERIFY(!cached(id, kY).isValid());
        QVERIFY(indexValue(root, id, xColumn()).isNull());
        QVERIFY(indexValue(root, id, yColumn()).isNull());
        QCOMPARE(indexRecordStamp(root, id), QJsonValue(QJsonObject()));
        QCOMPARE(engine(id).runCount(kCalcX), 0);
        QCOMPARE(engine(id).runCount(kCalcY), 0);
    }
    QCOMPARE(calculationRecordFiles(), QStringList());
}

void ResultColumnsTest::stampWrittenOnFlush()
{
    QVERIFY(fit("s1", kCalcY));

    const QJsonObject root = readIndex();
    QCOMPARE(indexRecordStamp(root, "s1"), stampOf({{kCalcY, QStringLiteral("y-v1")}}));
    QCOMPARE(indexRecordStamp(root, "s2"), QJsonValue(QJsonObject()));
    QCOMPARE(root.keys(), QStringList({QStringLiteral("calculationCompatibility"),
                                       QStringLiteral("columns"), QStringLiteral("sessions")}));
    for (const LogbookColumn &col : {descriptionColumn(), xColumn(), yColumn()})
        QCOMPARE(indexColumnEnvironment(root, col), logbookColumnEnvironment(col, CalculationRegistry::instance()));
}

void ResultColumnsTest::publishedResultIsCached_data()
{
    QTest::addColumn<bool>("queued");
    QTest::newRow("request") << false;
    QTest::newRow("queue") << true;
}

// The record written at the install drops the value at once; the next pass
// computes it from the engine, caches it and flushes it with its stamp.
void ResultColumnsTest::publishedResultIsCached()
{
    QFETCH(bool, queued);
    bool droppedAtInstall = false;
    bool othersKept = false;
    const auto check = [&] {
        droppedAtInstall = !isCached("s1", kY) && !pending("s1").contains(kY);
        othersKept = isCached("s1", kX) && isCached("s2", kX) && isCached("s2", kY);
    };

    QObject scope;      // owns the connection
    if (queued) {
        connect(m_queue.get(), &JobQueue::jobFinished, &scope, [&check](JobId, JobState) { check(); });
        QCOMPARE(m_queue->offer("s1", kCalcY).kind, JobQueue::OfferResult::Kind::Created);
        QVERIFY(waitIdle(*m_queue));
        QCOMPARE(m_queue->model()->record(0).state, JobState::Succeeded);
    } else {
        QCOMPARE(engine("s1").request(kCalcY).status, ResultStatus::Ok);
        check();
    }
    QVERIFY(droppedAtInstall);
    QVERIFY(othersKept);

    m_model->resetColumnWorkStats();
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(cached("s1", kY), QVariant(6.0));
    QCOMPARE(indexValue("s1", yColumn()), QJsonValue(6.0));
    QCOMPARE(indexRecordStamp("s1"), stampOf({{kCalcY, QStringLiteral("y-v1")}}));
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 0);
}

// ---- Unloaded rows --------------------------------------------------------------------

void ResultColumnsTest::restartShowsCachedValueWithoutLoading()
{
    QVERIFY(fit("s1", kCalcY));
    const QByteArray indexBytes = bytesOf(TestEnvironment::instance().indexPath());
    QVERIFY(!indexBytes.isEmpty());

    restart();
    QVERIFY(!isLoaded("s1"));
    QCOMPARE(cached("s1", kY), QVariant(6.0));
    QCOMPARE(cell("s1", kY), QStringLiteral("6"));

    m_model->resetColumnWorkStats();
    m_model->resetStoredResultStats();
    m_model->startColumnWorker();
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 0);
    QCOMPARE(m_model->columnWorkStats().valuesComputed, 0);
    QCOMPARE(stats().recordsRead, 0);
    QVERIFY(!isLoaded("s1"));
    QCOMPARE(bytesOf(TestEnvironment::instance().indexPath()), indexBytes);
}

void ResultColumnsTest::noRecordStaysUnavailableAfterRestart()
{
    restart();
    for (const char *id : {"s1", "s2"}) {
        QVERIFY(!isLoaded(id));
        for (int column : {kX, kY}) {
            QVERIFY(isCached(id, column));
            QVERIFY(!cached(id, column).isValid());
        }
    }
    m_model->resetColumnWorkStats();
    m_model->startColumnWorker();
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 0);
}

// ---- Records dropped ------------------------------------------------------------------

void ResultColumnsTest::inputChangeDropsCachedValue()
{
    QVERIFY(fit("s1", kCalcY));
    const QString path = recordPath("s1", kEncodedY);
    QVERIFY(QFileInfo(path).isFile());

    QVERIFY(m_model->updateAttribute("s1", QStringLiteral("Y_IN"), 4.0));
    // No event-loop pass in between
    QVERIFY(!QFileInfo::exists(path));
    QVERIFY(!isCached("s1", kY));
    QVERIFY(!pending("s1").contains(kY));

    QVERIFY(waitForIdle(*m_model));
    QVERIFY(isCached("s1", kY));
    QVERIFY(!cached("s1", kY).isValid());
    QVERIFY(indexValue("s1", yColumn()).isNull());
    QCOMPARE(indexRecordStamp("s1"), QJsonValue(QJsonObject()));
}

void ResultColumnsTest::onlyDependentColumnsDrop()
{
    QCOMPARE(engine("s1").request(kCalcX).status, ResultStatus::Ok);
    QVERIFY(fit("s1", kCalcY));
    QCOMPARE(cached("s1", kX), QVariant(QStringLiteral("x:d1")));
    const QByteArray xBytes = bytesOf(recordPath("s1", kEncodedX));
    QVERIFY(!xBytes.isEmpty());

    QVERIFY(m_model->updateAttribute("s1", QStringLiteral("Y_IN"), 7.0));
    QCOMPARE(cached("s1", kX), QVariant(QStringLiteral("x:d1")));
    QVERIFY(!isCached("s1", kY));
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(cached("s1", kX), QVariant(QStringLiteral("x:d1")));
    QCOMPARE(bytesOf(recordPath("s1", kEncodedX)), xBytes);
    QCOMPARE(indexRecordStamp("s1"), stampOf({{kCalcX, QString()}}));
    QCOMPARE(indexValue("s1", xColumn()), QJsonValue(QStringLiteral("x:d1")));
}

// A bulk edit on a stub changes an input of X with a temporary session, which
// reads no record and has no listener: the record stays, stale, and X is left
// missing - never computed from the record's presence, never pending. The
// column worker's temporary copy reads the record with the checks of a load,
// deletes it as stale and caches unavailable; the row is never loaded.
void ResultColumnsTest::staleRecordOnLoadDropsCachedValue()
{
    QCOMPARE(engine("s1").request(kCalcX).status, ResultStatus::Ok);
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(cached("s1", kX), QVariant(QStringLiteral("x:d1")));
    QCOMPARE(evict({"s1"}), QString());
    const QString path = recordPath("s1", kEncodedX);
    QVERIFY(QFileInfo(path).isFile());
    m_model->resetStoredResultStats();
    m_model->resetColumnWorkStats();
    QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);

    // The bulk edit alone (the worker gets no idle pass before the check):
    // nothing is read, and X waits for the worker
    QObject scope;
    bool seen = false;
    bool readAtBulkEdit = true;
    bool leftMissing = false;
    connect(&m_model->scheduler(), &IdleScheduler::activeTaskChanged, &scope, [&](int task, bool) {
        if (task != SessionModel::ColumnTask || seen)
            return;
        seen = true;    // before the worker's first step
        readAtBulkEdit = stats().recordsRead != 0;
        leftMissing = !isCached("s1", kX) && !pending("s1").contains(kX) && QFileInfo(path).isFile();
    });
    m_model->startBulkEdit({row("s1")}, kD, QStringLiteral("bulk"));
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(seen);
    QVERIFY(!readAtBulkEdit);
    QVERIFY(leftMissing);

    // The worker's copy found it stale
    QVERIFY(!isLoaded("s1"));
    QCOMPARE(loadedSpy.count(), 0);
    QCOMPARE(cached("s1", kD), QVariant(QStringLiteral("bulk")));
    QVERIFY(isCached("s1", kX));
    QVERIFY(!cached("s1", kX).isValid());
    QCOMPARE(pending("s1"), QSet<int>());
    QVERIFY(!QFileInfo::exists(path));
    QCOMPARE(stats().restoreCalls, 1);
    QCOMPARE(stats().recordsRead, 1);
    QCOMPARE(stats().staleRecordsDeleted, 1);
    QCOMPARE(stats().recordsRestored, 0);
    QCOMPARE(stats().recordsWritten, 0);
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 2);    // the bulk edit's, the worker's
    QVERIFY(indexValue("s1", xColumn()).isNull());
    QCOMPARE(indexRecordStamp("s1"), QJsonValue(QJsonObject()));

    // A load finds nothing left to restore, and agrees
    session("s1");
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(stats().staleRecordsDeleted, 1);
    QVERIFY(engine("s1").resultStatus(kCalcX) != std::optional<ResultStatus>(ResultStatus::Ok));
    QVERIFY(isCached("s1", kX));
    QVERIFY(!cached("s1", kX).isValid());
}

// A stub with a record and no value over it: the column worker restores the
// session's records into its temporary copy (the checks of a load) and caches
// the value computed from it, stamped, exactly what the loaded row gives. The
// row is never loaded, nothing runs, no job is created and no record is
// written. With another missing column too: still one copy and one restore.
void ResultColumnsTest::workerRestoresStoredResult()
{
    QCOMPARE(engine("s1").request(kCalcX).status, ResultStatus::Ok);
    QVERIFY(waitForIdle(*m_model));
    const QString path = recordPath("s1", kEncodedX);
    const QByteArray recordBytes = bytesOf(path);
    QVERIFY(!recordBytes.isEmpty());
    resetModel();
    QVERIFY(editIndex([](QJsonObject &root) { removeIndexValue(root, "s1", xColumn()); }));
    restart();
    QVERIFY(!isCached("s1", kX));
    QCOMPARE(pending("s1"), QSet<int>());

    m_model->resetColumnWorkStats();
    m_model->resetStoredResultStats();
    QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);
    m_model->startColumnWorker();
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(cached("s1", kX), QVariant(QStringLiteral("x:d1")));
    QCOMPARE(cell("s1", kX), QStringLiteral("x:d1"));
    QCOMPARE(pending("s1"), QSet<int>());
    QVERIFY(!isLoaded("s1"));
    QCOMPARE(loadedSpy.count(), 0);
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 1);
    QCOMPARE(m_model->columnWorkStats().valuesComputed, 1);
    QCOMPARE(m_model->columnWorkStats().calculationRuns, 0);
    QCOMPARE(stats().restoreCalls, 1);
    QCOMPARE(stats().recordsRead, 1);
    QCOMPARE(stats().recordsRestored, 1);
    QCOMPARE(stats().staleRecordsDeleted, 0);
    QCOMPARE(stats().recordsWritten, 0);
    QCOMPARE(m_queue->model()->rowCount(), 0);
    QCOMPARE(bytesOf(path), recordBytes);
    QCOMPARE(LogbookManager::instance().unconfirmedCalculationRecords("s1"), QSet<QString>());
    QCOMPARE(indexValue("s1", xColumn()), QJsonValue(QStringLiteral("x:d1")));
    QCOMPARE(indexRecordStamp("s1"), stampOf({{kCalcX, QString()}}));

    // A load restores the same result: the loaded row agrees
    session("s1");
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(engine("s1").runCount(kCalcX), 0);
    QCOMPARE(engine("s1").attribute(QStringLiteral("X_OUT")), QVariant(QStringLiteral("x:d1")));
    QCOMPARE(cached("s1", kX), QVariant(QStringLiteral("x:d1")));

    // A stub with a record and another missing column: one temporary copy and
    // one restore, which fill both
    resetModel();
    QVERIFY(editIndex([](QJsonObject &root) {
        removeIndexValue(root, "s1", xColumn());
        removeIndexValue(root, "s1", descriptionColumn());
    }));
    restart();
    m_model->resetColumnWorkStats();
    m_model->resetStoredResultStats();
    m_model->startColumnWorker();
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 1);
    QCOMPARE(stats().restoreCalls, 1);
    QCOMPARE(stats().recordsRestored, 1);
    QCOMPARE(cached("s1", kD), QVariant(QStringLiteral("d1")));
    QCOMPARE(cached("s1", kX), QVariant(QStringLiteral("x:d1")));
    QCOMPARE(pending("s1"), QSet<int>());
    QVERIFY(!isLoaded("s1"));
    QCOMPARE(bytesOf(path), recordBytes);
}

void ResultColumnsTest::workerSkipsUnreadableRecord_data()
{
    QTest::addColumn<int>("mechanism");
    QTest::newRow("locked without sharing") << int(UnreadableFile::Mechanism::LockedWithoutSharing);
    QTest::newRow("no read permission") << int(UnreadableFile::Mechanism::NoReadPermission);
}

// A record the worker's copy cannot read is skipped with the checks of a
// load: kept, neither restored nor deleted. The value over it is not cached
// (in the row or index.json) but pending, without loading the row; the
// worker does not come back to it, and the copy's mark goes with the copy.
// Once readable, a load restores it.
void ResultColumnsTest::workerSkipsUnreadableRecord()
{
    QFETCH(int, mechanism);
    LogbookManager &logbook = LogbookManager::instance();

    QVERIFY(fit("s1", kCalcY));
    const QString path = recordPath("s1", kEncodedY);
    resetModel();
    QVERIFY(editIndex([](QJsonObject &root) { removeIndexValue(root, "s1", yColumn()); }));
    UnreadableFile unreadable(path, UnreadableFile::Mechanism(mechanism));
    if (!unreadable.skipReason().isEmpty())
        QSKIP(qPrintable(unreadable.skipReason()));
    restart();
    QVERIFY(!isCached("s1", kY));

    m_model->resetColumnWorkStats();
    m_model->resetStoredResultStats();
    {
        WarningCapture warnings;    // the skip warns
        m_model->startColumnWorker();
        QVERIFY(waitForIdle(*m_model));
        QCOMPARE(warnings.count(QStringLiteral("skipped (kept for the next load)")), 1);
    }
    QCOMPARE(pending("s1"), QSet<int>({kY}));
    QVERIFY(!isCached("s1", kY));
    QCOMPARE(cell("s1", kY), QString());
    QVERIFY(!isLoaded("s1"));
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 1);
    QCOMPARE(stats().restoreCalls, 1);
    QCOMPARE(stats().recordsSkipped, 1);
    QCOMPARE(stats().recordsRestored, 0);
    QCOMPARE(stats().staleRecordsDeleted, 0);
    QCOMPARE(logbook.unconfirmedCalculationRecords("s1"), QSet<QString>());
    QVERIFY(logbook.flushIndex());
    QVERIFY(indexValue("s1", yColumn()).isUndefined());
    QCOMPARE(indexValue("s1", descriptionColumn()), QJsonValue(QStringLiteral("d1")));
    QVERIFY(indexValue("s2", yColumn()).isNull());

    // The worker passes the pending value over
    m_model->resetColumnWorkStats();
    m_model->startColumnWorker();
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 0);

    // Readable again: the next load restores it
    QVERIFY(unreadable.release());
    session("s1");
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(engine("s1").resultStatus(kCalcY), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(engine("s1").runCount(kCalcY), 0);
    QCOMPARE(pending("s1"), QSet<int>());
    QCOMPARE(cached("s1", kY), QVariant(6.0));
    QVERIFY(logbook.flushIndex());
    QCOMPARE(indexValue("s1", yColumn()), QJsonValue(6.0));
    QCOMPARE(indexRecordStamp("s1"), stampOf({{kCalcY, QStringLiteral("y-v1")}}));
}

// A stub whose missing columns are all on demand: one temporary copy computes
// them, and no record is read although the session has one.
void ResultColumnsTest::onDemandColumnsReadNoRecord()
{
    QCOMPARE(engine("s1").request(kCalcX).status, ResultStatus::Ok);
    QVERIFY(waitForIdle(*m_model));
    resetModel();
    QVERIFY(editIndex([](QJsonObject &root) { removeIndexValue(root, "s1", descriptionColumn()); }));
    restart();
    QVERIFY(!isCached("s1", kD));
    QCOMPARE(cached("s1", kX), QVariant(QStringLiteral("x:d1")));
    QCOMPARE(LogbookManager::instance().knownCalculationRecords("s1"), QSet<QString>({kCalcX}));

    m_model->resetColumnWorkStats();
    m_model->resetStoredResultStats();
    m_model->startColumnWorker();
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(cached("s1", kD), QVariant(QStringLiteral("d1")));
    QCOMPARE(cached("s1", kX), QVariant(QStringLiteral("x:d1")));
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 1);
    QCOMPARE(stats().restoreCalls, 0);
    QCOMPARE(stats().recordListings, 0);
    QCOMPARE(stats().recordsRead, 0);
    QVERIFY(!isLoaded("s1"));
}

// ---- Crash points ---------------------------------------------------------------------

// The stamp on disk does not list X: the write needs no flush first, and the
// start-up check drops the value the index still holds. The column worker
// fills it from the record, without loading the row.
void ResultColumnsTest::crashAfterRecordWrite()
{
    const QByteArray indexBytes = bytesOf(TestEnvironment::instance().indexPath());
    QCOMPARE(engine("s1").request(kCalcX).status, ResultStatus::Ok);
    QCOMPARE(bytesOf(TestEnvironment::instance().indexPath()), indexBytes);
    crash();

    const QString xKey = LogbookManager::columnDefKey(xColumn());
    QVERIFY(!LogbookManager::instance().cachedValuesForSession("s1").contains(xKey));
    QVERIFY(LogbookManager::instance().cachedValuesForSession("s2").contains(xKey));
    QVERIFY(LogbookManager::instance().cachedValuesForSession("s2").value(xKey).isNull());

    m_model->resetColumnWorkStats();
    m_model->resetStoredResultStats();
    m_model->startColumnWorker();
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(pending("s1"), QSet<int>());
    QCOMPARE(cached("s1", kX), QVariant(QStringLiteral("x:d1")));
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 1);
    QCOMPARE(stats().recordsRestored, 1);
    QVERIFY(!isLoaded("s1"));
    QCOMPARE(indexValue("s1", xColumn()), QJsonValue(QStringLiteral("x:d1")));
    QCOMPARE(indexRecordStamp("s1"), stampOf({{kCalcX, QString()}}));

    session("s1");
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(cached("s1", kX), QVariant(QStringLiteral("x:d1")));
}

// The stamp on disk lists X under a value; the record is gone.
void ResultColumnsTest::crashAfterRecordDelete()
{
    QCOMPARE(engine("s1").request(kCalcX).status, ResultStatus::Ok);
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(indexValue("s1", xColumn()), QJsonValue(QStringLiteral("x:d1")));
    QCOMPARE(indexRecordStamp("s1"), stampOf({{kCalcX, QString()}}));
    const QString csv = sessionFilePath("s1");

    QVERIFY(m_model->updateAttribute("s1", QStringLiteral("_DESCRIPTION"), QStringLiteral("e")));
    QVERIFY(!QFileInfo::exists(recordPath("s1", kEncodedX)));
    crash();

    const QString xKey = LogbookManager::columnDefKey(xColumn());
    QVERIFY(!LogbookManager::instance().cachedValuesForSession("s1").contains(xKey));

    m_model->resetColumnWorkStats();
    m_model->startColumnWorker();
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(isCached("s1", kX));
    QVERIFY(!cached("s1", kX).isValid());
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 0);
    QVERIFY(bytesOf(csv).contains("_DESCRIPTION,d1\n"));      // the edit was not saved

    // What a reload gives: no record, so X is not requested
    QVERIFY(engine("s1").resultStatus(kCalcX) != std::optional<ResultStatus>(ResultStatus::Ok));
}

// Delete then write of the same pair with no flush in between: the write
// flushes the index first, without the value and without X in the stamp.
void ResultColumnsTest::rewriteAfterDropFlushesIndexFirst()
{
    QCOMPARE(engine("s1").request(kCalcX).status, ResultStatus::Ok);
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(indexValue("s1", xColumn()), QJsonValue(QStringLiteral("x:d1")));
    const QString path = recordPath("s1", kEncodedX);
    const QString csv = sessionFilePath("s1");

    // No event-loop pass in between
    QVERIFY(m_model->updateAttribute("s1", QStringLiteral("_DESCRIPTION"), QStringLiteral("e")));
    QVERIFY(!QFileInfo::exists(path));
    QCOMPARE(engine("s1").request(kCalcX).status, ResultStatus::Ok);

    const QJsonObject root = readIndex();
    QVERIFY(indexValue(root, "s1", xColumn()).isUndefined());
    QVERIFY(indexRecordStamp(root, "s1").isObject());
    QVERIFY(!indexRecordStamp(root, "s1").toObject().contains(kCalcX));
    QVERIFY(QFileInfo(path).isFile());

    // The record describes _DESCRIPTION "e", which was never saved: the
    // worker's copy finds it stale, deletes it and caches unavailable
    crash();
    m_model->resetStoredResultStats();
    m_model->startColumnWorker();
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(bytesOf(csv).contains("_DESCRIPTION,d1\n"));      // the edit was not saved
    QCOMPARE(stats().staleRecordsDeleted, 1);
    QVERIFY(!QFileInfo::exists(path));
    QCOMPARE(pending("s1"), QSet<int>());
    QVERIFY(isCached("s1", kX));
    QVERIFY(!cached("s1", kX).isValid());
    QVERIFY(!isLoaded("s1"));

    // What a load gives agrees
    session("s1");
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(stats().staleRecordsDeleted, 1);
    QVERIFY(engine("s1").resultStatus(kCalcX) != std::optional<ResultStatus>(ResultStatus::Ok));
    QVERIFY(isCached("s1", kX));
    QVERIFY(!cached("s1", kX).isValid());
}

void ResultColumnsTest::writeAfterStartupDropFlushesIndexFirst_data()
{
    QTest::addColumn<bool>("environmentChanged");
    QTest::newRow("droppedAtStart") << false;
    QTest::newRow("indexNotValid") << true;
}

// index.json on disk holds a value over X with X in the stamp, which the
// start did not keep: the record was deleted before a crash (the check drops
// the value), or the index was written in another environment of X's column
// (a second candidate for _DESCRIPTION, which X reads: X's value is not kept;
// the record itself stays valid - the stored _DESCRIPTION still answers - and
// is restored at the load, then dropped by the edit). The first write of X in that run still flushes the index first, so
// that after a crash (and back in the first environment) the value stays
// dropped rather than being checked against the new record.
void ResultColumnsTest::writeAfterStartupDropFlushesIndexFirst()
{
    QFETCH(bool, environmentChanged);
    const QString indexPath = TestEnvironment::instance().indexPath();
    const QString xKey = LogbookManager::columnDefKey(xColumn());
    const QString path = recordPath("s1", kEncodedX);

    QCOMPARE(engine("s1").request(kCalcX).status, ResultStatus::Ok);
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(indexValue("s1", xColumn()), QJsonValue(QStringLiteral("x:d1")));
    QCOMPARE(indexRecordStamp("s1"), stampOf({{kCalcX, QString()}}));

    bool extraRegistered = false;
    const auto unregister = qScopeGuard([this, &extraRegistered] {
        if (extraRegistered) {
            resetModel();
            CalculationRegistry::instance().unregister(kExtra, CalculationRegistry::Removal::Change);
        }
    });
    if (environmentChanged) {
        // Another environment of X's column for the next run (registered
        // with no model)
        resetModel();
        CalculationDescriptor extra;
        extra.id = kExtra;
        extra.outputs = {DependencyKey::attribute(QStringLiteral("_DESCRIPTION"))};
        extra.compute = [](const EvaluationContext &) {
            return CalculationResult().setAttribute(QStringLiteral("_DESCRIPTION"), QStringLiteral("extra"));
        };
        QVERIFY(CalculationRegistry::instance().registerCalculation(extra));
        extraRegistered = true;
    } else {
        QVERIFY(m_model->updateAttribute("s1", QStringLiteral("_DESCRIPTION"), QStringLiteral("e")));
        QVERIFY(!QFileInfo::exists(path));
    }
    const QByteArray indexBytes = bytesOf(indexPath);
    crash();
    QVERIFY(!LogbookManager::instance().cachedValuesForSession("s1").contains(xKey));
    QCOMPARE(LogbookManager::instance().cachedValuesDiscardedOnLoad(), environmentChanged);

    // Before any flush: an edit of X's input and a new fit
    m_model->resetStoredResultStats();
    QVERIFY(m_model->updateAttribute("s1", QStringLiteral("_DESCRIPTION"), QStringLiteral("f")));
    if (environmentChanged) {
        QCOMPARE(stats().recordsRestored, 1);
        QCOMPARE(stats().staleRecordsDeleted, 0);
        QCOMPARE(stats().droppedRecordsDeleted, 1);
    } else {
        QCOMPARE(stats().recordsRead, 0);
    }
    QCOMPARE(bytesOf(indexPath), indexBytes);
    QCOMPARE(engine("s1").request(kCalcX).status, ResultStatus::Ok);
    QVERIFY(QFileInfo(path).isFile());
    {
        const QJsonObject root = readIndex();
        QVERIFY(indexValue(root, "s1", xColumn()).isUndefined());
        QVERIFY(indexRecordStamp(root, "s1").isObject());
        QVERIFY(!indexRecordStamp(root, "s1").toObject().contains(kCalcX));
    }

    // A crash, then a start in the first environment: the old value is not shown
    if (environmentChanged) {
        resetModel();
        QVERIFY(CalculationRegistry::instance().unregister(kExtra, CalculationRegistry::Removal::Change));
        extraRegistered = false;
    }
    crash();
    QVERIFY(!LogbookManager::instance().cachedValuesForSession("s1").contains(xKey));
    QVERIFY(!isLoaded("s1"));
    QVERIFY(!isCached("s1", kX));
    QCOMPARE(cell("s1", kX), QString());
}

// ---- Old indexes, versions, failures ----------------------------------------------------

void ResultColumnsTest::oldIndexWithoutStamp_data()
{
    QTest::addColumn<bool>("withRecord");
    QTest::newRow("noRecord") << false;
    QTest::newRow("withRecord") << true;
}

// An older build wrote no stamp and cached explicit-backed columns as
// unavailable: such a value is kept only when the session has no record.
void ResultColumnsTest::oldIndexWithoutStamp()
{
    QFETCH(bool, withRecord);
    if (withRecord) {
        QCOMPARE(engine("s1").request(kCalcX).status, ResultStatus::Ok);
        QVERIFY(waitForIdle(*m_model));
    }
    resetModel();
    QVERIFY(editIndex([](QJsonObject &root) {
        QJsonObject sessions = root[QStringLiteral("sessions")].toObject();
        for (auto it = sessions.begin(); it != sessions.end(); ++it) {
            QJsonObject entry = it.value().toObject();
            entry.remove(QStringLiteral("records"));
            it.value() = entry;
        }
        root[QStringLiteral("sessions")] = sessions;
        setIndexValue(root, "s1", xColumn(), QJsonValue::Null);
        setIndexValue(root, "s2", xColumn(), QJsonValue::Null);
    }));
    QVERIFY(indexRecordStamp("s1").isUndefined());
    restart();

    m_model->resetColumnWorkStats();
    if (withRecord) {
        // Dropped: the worker fills it from the record, without loading the row
        QVERIFY(!isCached("s1", kX));
        m_model->startColumnWorker();
        QVERIFY(waitForIdle(*m_model));
        QCOMPARE(pending("s1"), QSet<int>());
        QCOMPARE(cached("s1", kX), QVariant(QStringLiteral("x:d1")));
        QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 1);
        QVERIFY(!isLoaded("s1"));
        session("s1");
        QVERIFY(waitForIdle(*m_model));
        QCOMPARE(cached("s1", kX), QVariant(QStringLiteral("x:d1")));
    } else {
        QVERIFY(isCached("s1", kX));
        QVERIFY(!cached("s1", kX).isValid());
        m_model->startColumnWorker();
        QVERIFY(waitForIdle(*m_model));
        QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 0);
    }
    QVERIFY(isCached("s2", kX));
    QVERIFY(!cached("s2", kX).isValid());

    QVERIFY(LogbookManager::instance().flushIndex());
    QVERIFY(allEntriesHaveStamp(readIndex()));
}

// A stamp whose result version is not the current one: the value goes; the
// record itself carries the current version and restores (into the worker's
// copy, which fills the value, and at a load).
void ResultColumnsTest::resultVersionChangeDropsCachedValue()
{
    QVERIFY(fit("s1", kCalcY));
    resetModel();
    QVERIFY(editIndex([](QJsonObject &root) {
        setIndexStamp(root, "s1", QJsonObject{{kCalcY, QStringLiteral("y-v0")}});
    }));
    restart();

    QVERIFY(!isCached("s1", kY));
    m_model->resetStoredResultStats();
    m_model->startColumnWorker();
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(pending("s1"), QSet<int>());
    QCOMPARE(cached("s1", kY), QVariant(6.0));
    QCOMPARE(stats().recordsRestored, 1);
    QVERIFY(!isLoaded("s1"));
    QCOMPARE(indexRecordStamp("s1"), stampOf({{kCalcY, QStringLiteral("y-v1")}}));
    QVERIFY(isCached("s2", kY));
    QVERIFY(!cached("s2", kY).isValid());

    session("s1");
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(engine("s1").resultStatus(kCalcY), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(engine("s1").runCount(kCalcY), 0);
    QCOMPARE(cached("s1", kY), QVariant(6.0));
}

// A directory at the record's path: the value computed from the engine stays
// out of index.json (and the calculation out of the stamp) until the row is
// evicted; then the disk is the truth.
void ResultColumnsTest::writeFailureKeepsValueOutOfIndex()
{
    const QString path = recordPath("s1", kEncodedY);
    QVERIFY(QDir().mkpath(path));
    const auto removeDirectory = qScopeGuard([path] { QDir().rmdir(path); });
    LogbookManager &logbook = LogbookManager::instance();

    {
        WarningCapture warnings;
        QCOMPARE(engine("s1").request(kCalcY).status, ResultStatus::Ok);
        QCOMPARE(warnings.count(QStringLiteral("not written")), 1);
    }
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(logbook.flushIndex());

    QCOMPARE(cell("s1", kY), QStringLiteral("6"));
    QCOMPARE(logbook.unconfirmedCalculationRecords("s1"), QSet<QString>({kCalcY}));
    {
        const QJsonObject root = readIndex();
        QVERIFY(indexValue(root, "s1", yColumn()).isUndefined());
        QVERIFY(indexRecordStamp(root, "s1").isObject());
        QVERIFY(!indexRecordStamp(root, "s1").toObject().contains(kCalcY));
    }

    QCOMPARE(evict({"s1"}), QString());
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(isCached("s1", kY));
    QVERIFY(!cached("s1", kY).isValid());
    QVERIFY(indexValue("s1", yColumn()).isNull());
    QCOMPARE(logbook.unconfirmedCalculationRecords("s1"), QSet<QString>());
}

// A session created by an import has its record under the reserved stem; the
// index lists it (with its value and stamp) from the first save on.
void ResultColumnsTest::recordBeforeFirstSaveIsCachedAfterSave()
{
    QCOMPARE(m_model->mergeSessions({columnSession(QStringLiteral("n1"), QStringLiteral("dn"), 3.0)})
                 .at(0).outcome, MergeResult::Outcome::Created);
    {
        WarningCapture warnings;
        QCOMPARE(engine("n1").request(kCalcY).status, ResultStatus::Ok);
        QCOMPARE(warnings.messages(), QStringList());
    }

    QVERIFY(LogbookManager::instance().flushIndex());
    QVERIFY(!readIndex()[QStringLiteral("sessions")].toObject().contains(QStringLiteral("n1")));

    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(indexValue("n1", yColumn()), QJsonValue(6.0));
    QCOMPARE(indexRecordStamp("n1"), stampOf({{kCalcY, QStringLiteral("y-v1")}}));

    restart();
    QVERIFY(!isLoaded("n1"));
    QCOMPARE(cached("n1", kY), QVariant(6.0));
    m_model->resetColumnWorkStats();
    m_model->startColumnWorker();
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 0);
}

// ---- Environment changes --------------------------------------------------------------

// A registry change discards the cached values of exactly the columns whose
// closure it reaches. One that reaches none (a new, unrelated name) keeps
// everything - Y of the unloaded s1 stays cached from its record, neither
// pending nor loaded. A provider of Y_IN (tried after the stored Y_IN) reaches
// Y only: the worker computes Y again (for s1, which has a record, from its
// stored result restored into a temporary copy - the row stays unloaded; for
// the loaded s2, unavailable from its engine) and D and X keep their values.
void ResultColumnsTest::environmentChangeDiscardsReachedColumn()
{
    QVERIFY(fit("s1", kCalcY));
    QCOMPARE(evict({"s1"}), QString());
    session("s2");      // loaded (the idle saver's touch put it in the LRU, so the eviction took it too)
    QVERIFY(isLoaded("s2"));
    LogbookManager &logbook = LogbookManager::instance();
    CalculationRegistry &registry = CalculationRegistry::instance();

    const auto unregister = qScopeGuard([this] {
        resetModel();
        CalculationRegistry::instance().unregister(kExtra, CalculationRegistry::Removal::Change);
        CalculationRegistry::instance().unregister(kShadow, CalculationRegistry::Removal::Change);
    });

    // 1. Unrelated: nothing is dropped, nothing is loaded
    CalculationDescriptor extra;
    extra.id = kExtra;
    extra.outputs = {DependencyKey::attribute(QStringLiteral("_COLUMNS_EXTRA"))};
    extra.compute = [](const EvaluationContext &) {
        return CalculationResult().setAttribute(QStringLiteral("_COLUMNS_EXTRA"), 1);
    };
    const QString yEnvironment = logbook.columnEnvironment(yColumn());
    QVERIFY(registry.registerCalculation(extra));
    m_model->resetColumnWorkStats();
    m_model->flushPendingInvalidations();
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(!isLoaded("s1"));
    QCOMPARE(pending("s1"), QSet<int>());
    QCOMPARE(cached("s1", kY), QVariant(6.0));
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 0);
    QCOMPARE(m_model->columnWorkStats().valuesComputed, 0);
    QCOMPARE(logbook.columnEnvironment(yColumn()), yEnvironment);
    QCOMPARE(indexValue("s1", yColumn()), QJsonValue(6.0));

    // 2. A provider of Y_IN reaches Y's closure, and nothing else's
    CalculationDescriptor shadow;
    shadow.id = kShadow;
    shadow.outputs = {DependencyKey::attribute(QStringLiteral("Y_IN"))};
    shadow.compute = [](const EvaluationContext &) {
        return CalculationResult().setAttribute(QStringLiteral("Y_IN"), 3.0);
    };
    const QString xEnvironment = logbook.columnEnvironment(xColumn());
    QVERIFY(registry.registerCalculation(shadow));
    m_model->resetColumnWorkStats();
    m_model->resetStoredResultStats();
    m_model->flushPendingInvalidations();
    QVERIFY(waitForIdle(*m_model));

    QVERIFY(logbook.columnEnvironment(yColumn()) != yEnvironment);
    QCOMPARE(logbook.columnEnvironment(xColumn()), xEnvironment);
    QVERIFY(!isLoaded("s1"));
    QCOMPARE(pending("s1"), QSet<int>());
    QCOMPARE(cached("s1", kY), QVariant(6.0));
    QCOMPARE(cached("s1", kD), QVariant(QStringLiteral("d1")));
    QVERIFY(isCached("s1", kX));
    QVERIFY(!cached("s1", kX).isValid());
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 1);     // s1's copy
    QCOMPARE(m_model->columnWorkStats().valuesComputed, 2);     // Y of s1 from the copy, Y of s2 from its engine
    QCOMPARE(stats().recordsRestored, 1);
    QCOMPARE(stats().staleRecordsDeleted, 0);
    QVERIFY(isCached("s2", kY));
    QVERIFY(!cached("s2", kY).isValid());
    QCOMPARE(indexValue("s1", yColumn()), QJsonValue(6.0));
    QCOMPARE(indexColumnEnvironment(yColumn()), logbook.columnEnvironment(yColumn()));
    QVERIFY(indexValue("s1", xColumn()).isNull());
    QCOMPARE(indexValue("s1", descriptionColumn()), QJsonValue(QStringLiteral("d1")));

    // The record itself stays valid: the next load restores it
    m_model->resetStoredResultStats();
    session("s1");
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(engine("s1").resultStatus(kCalcY), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(engine("s1").runCount(kCalcY), 0);
    QCOMPARE(stats().recordsRestored, 1);
    QCOMPARE(stats().staleRecordsDeleted, 0);
    QCOMPARE(cached("s1", kY), QVariant(6.0));
    QVERIFY(LogbookManager::instance().flushIndex());
    QCOMPARE(indexValue("s1", yColumn()), QJsonValue(6.0));
    QCOMPARE(indexRecordStamp("s1"), stampOf({{kCalcY, QStringLiteral("y-v1")}}));
}

// A registry change that does not reach Y leaves the installed result and its
// record alone, so a loaded row's values over it stay cacheable: nothing is
// unconfirmed, whether the environment comes back within the pass (A -> B ->
// A) or stays changed (the cached values are discarded and recomputed from
// the engine). A provider of Y_IN registered behind the stored Y_IN does not
// reach Y either. A registry change that does (the removal of that provider
// once Y reads Y_IN from it) drops the result and deletes its record at once.
void ResultColumnsTest::registryChangeKeepsLoadedRowConfirmed()
{
    QVERIFY(fit("s1", kCalcY));
    QCOMPARE(indexValue("s1", yColumn()), QJsonValue(6.0));
    LogbookManager &logbook = LogbookManager::instance();
    CalculationRegistry &registry = CalculationRegistry::instance();

    const auto unregister = qScopeGuard([this] {
        resetModel();
        CalculationRegistry::instance().unregister(kShadow, CalculationRegistry::Removal::Change);
        CalculationRegistry::instance().unregister(kExtra, CalculationRegistry::Removal::Change);
    });
    CalculationDescriptor extra;
    extra.id = kExtra;
    extra.outputs = {DependencyKey::attribute(QStringLiteral("_COLUMNS_EXTRA"))};
    extra.compute = [](const EvaluationContext &) {
        return CalculationResult().setAttribute(QStringLiteral("_COLUMNS_EXTRA"), 1);
    };

    // A -> B -> A within one pass
    QVERIFY(registry.registerCalculation(extra));
    QVERIFY(registry.unregister(kExtra, CalculationRegistry::Removal::Change));
    m_model->flushPendingInvalidations();
    QCOMPARE(logbook.unconfirmedCalculationRecords("s1"), QSet<QString>());
    QCOMPARE(engine("s1").resultStatus(kCalcY), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(engine("s1").runCount(kCalcY), 1);
    QVERIFY(logbook.flushIndex());
    QCOMPARE(indexValue("s1", yColumn()), QJsonValue(6.0));
    QCOMPARE(indexRecordStamp("s1"), stampOf({{kCalcY, QStringLiteral("y-v1")}}));

    // An environment change that stays
    QVERIFY(registry.registerCalculation(extra));
    m_model->flushPendingInvalidations();
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(logbook.unconfirmedCalculationRecords("s1"), QSet<QString>());
    QCOMPARE(engine("s1").resultStatus(kCalcY), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(engine("s1").runCount(kCalcY), 1);
    QCOMPARE(cached("s1", kY), QVariant(6.0));      // kept: no column's closure reaches the new name
    QVERIFY(logbook.flushIndex());
    QCOMPARE(indexValue("s1", yColumn()), QJsonValue(6.0));
    QCOMPARE(indexRecordStamp("s1"), stampOf({{kCalcY, QStringLiteral("y-v1")}}));

    // A provider of Y_IN registered: the stored Y_IN still wins, so it does
    // not reach Y either
    const QString path = recordPath("s1", kEncodedY);
    const QByteArray recordBytes = [&path] {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }();
    QVERIFY(!recordBytes.isEmpty());
    m_model->resetStoredResultStats();
    CalculationDescriptor shadow;
    shadow.id = kShadow;
    shadow.outputs = {DependencyKey::attribute(QStringLiteral("Y_IN"))};
    shadow.compute = [](const EvaluationContext &) {
        return CalculationResult().setAttribute(QStringLiteral("Y_IN"), 3.0);
    };
    QVERIFY(registry.registerCalculation(shadow));
    m_model->flushPendingInvalidations();
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(engine("s1").resultStatus(kCalcY), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(stats().droppedRecordsDeleted, 0);
    QCOMPARE(logbook.unconfirmedCalculationRecords("s1"), QSet<QString>());
    QVERIFY(logbook.flushIndex());
    QCOMPARE(indexValue("s1", yColumn()), QJsonValue(6.0));
    QFile record(path);
    QVERIFY(record.open(QIODevice::ReadOnly));
    QCOMPARE(record.readAll(), recordBytes);
    record.close();

    // A registry change reaching Y: once the stored Y_IN is gone, Y reads it
    // from the provider, and removing the provider drops the result and
    // deletes its record at once
    QVERIFY(m_model->removeAttribute("s1", QStringLiteral("Y_IN")));
    QVERIFY(fit("s1", kCalcY));
    QCOMPARE(engine("s1").attribute(QStringLiteral("Y_OUT")), QVariant(6.0));
    QVERIFY(QFileInfo(path).isFile());
    QVERIFY(logbook.flushIndex());
    QCOMPARE(indexValue("s1", yColumn()), QJsonValue(6.0));
    m_model->resetStoredResultStats();
    QVERIFY(registry.unregister(kShadow, CalculationRegistry::Removal::Change));
    QVERIFY(!QFileInfo::exists(path));
    QCOMPARE(stats().droppedRecordsDeleted, 1);
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(logbook.flushIndex());
    QVERIFY(indexValue("s1", yColumn()).isNull());
    QCOMPARE(indexRecordStamp("s1"), QJsonValue(QJsonObject()));
}

void ResultColumnsTest::skippedRecordValuesStayOutOfIndex_data()
{
    QTest::addColumn<int>("mechanism");
    QTest::addColumn<bool>("viaRestart");

    QTest::newRow("directory") << int(UnreadableFile::Mechanism::Directory) << false;
    QTest::newRow("locked without sharing") << int(UnreadableFile::Mechanism::LockedWithoutSharing) << false;
    QTest::newRow("locked without sharing, restart") << int(UnreadableFile::Mechanism::LockedWithoutSharing) << true;
    QTest::newRow("no read permission") << int(UnreadableFile::Mechanism::NoReadPermission) << false;
    QTest::newRow("no read permission, restart") << int(UnreadableFile::Mechanism::NoReadPermission) << true;
}

// A record skipped at a load (it cannot be read): the loaded row's value over
// it is unavailable and never reaches index.json, nor does the calculation
// reach the stamp; after eviction the column worker's copy skips it again
// and leaves the column pending; once readable, the next load restores it and
// the value is cached again.
void ResultColumnsTest::skippedRecordValuesStayOutOfIndex()
{
    QFETCH(int, mechanism);
    QFETCH(bool, viaRestart);
    LogbookManager &logbook = LogbookManager::instance();

    QVERIFY(fit("s1", kCalcY));
    QCOMPARE(indexValue("s1", yColumn()), QJsonValue(6.0));
    QCOMPARE(indexRecordStamp("s1"), stampOf({{kCalcY, QStringLiteral("y-v1")}}));
    QCOMPARE(evict({"s1"}), QString());
    QCOMPARE(cached("s1", kY), QVariant(6.0));

    UnreadableFile unreadable(recordPath("s1", kEncodedY), UnreadableFile::Mechanism(mechanism));
    if (!unreadable.skipReason().isEmpty())
        QSKIP(qPrintable(unreadable.skipReason()));
    if (viaRestart) {
        restart();
        QCOMPARE(cached("s1", kY), QVariant(6.0));      // the kept value agrees with the record on disk
    }

    m_model->resetStoredResultStats();
    {
        WarningCapture warnings;    // the skip warns
        session("s1");
        QVERIFY(!isCached("s1", kY));
        QCOMPARE(logbook.unconfirmedCalculationRecords("s1"), QSet<QString>({kCalcY}));
        QCOMPARE(stats().recordsSkipped, 1);
        QCOMPARE(stats().staleRecordsDeleted, 0);
        QCOMPARE(warnings.count(QStringLiteral("skipped (kept for the next load)")), 1);
    }

    QVERIFY(waitForIdle(*m_model));
    QVERIFY(isCached("s1", kY));
    QVERIFY(!cached("s1", kY).isValid());
    QCOMPARE(cell("s1", kY), QString());
    QVERIFY(logbook.flushIndex());
    {
        const QJsonObject root = readIndex();
        QVERIFY(indexValue(root, "s1", yColumn()).isUndefined());
        QVERIFY(indexRecordStamp(root, "s1").isObject());
        QVERIFY(!indexRecordStamp(root, "s1").toObject().contains(kCalcY));
        QCOMPARE(indexValue(root, "s1", descriptionColumn()), QJsonValue(QStringLiteral("d1")));
        QVERIFY(indexValue(root, "s2", yColumn()).isNull());
        QCOMPARE(indexRecordStamp(root, "s2"), QJsonValue(QJsonObject()));
    }

    // Eviction: the mark goes with the engine; the worker's copy skips the
    // record again, and the column waits for a load
    m_model->resetColumnWorkStats();
    m_model->resetStoredResultStats();
    {
        WarningCapture warnings;
        QCOMPARE(evict({"s1"}), QString());
        QVERIFY(waitForIdle(*m_model));
    }
    QCOMPARE(logbook.unconfirmedCalculationRecords("s1"), QSet<QString>());
    QCOMPARE(pending("s1"), QSet<int>({kY}));
    QVERIFY(!isCached("s1", kY));
    QVERIFY(!isLoaded("s1"));
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 1);
    QCOMPARE(stats().recordsSkipped, 1);
    QCOMPARE(stats().staleRecordsDeleted, 0);
    QVERIFY(logbook.flushIndex());
    QVERIFY(indexValue("s1", yColumn()).isUndefined());

    // Readable again
    QVERIFY(unreadable.release());
    session("s1");
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(engine("s1").resultStatus(kCalcY), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(engine("s1").runCount(kCalcY), 0);
    QCOMPARE(cached("s1", kY), QVariant(6.0));
    QVERIFY(logbook.flushIndex());
    QCOMPARE(indexValue("s1", yColumn()), QJsonValue(6.0));
    QCOMPARE(indexRecordStamp("s1"), stampOf({{kCalcY, QStringLiteral("y-v1")}}));
}

// ---- The manager ----------------------------------------------------------------------

void ResultColumnsTest::managerDropsDependentValues()
{
    resetModel();
    LogbookManager &logbook = LogbookManager::instance();

    std::optional<SessionData> copy = logbook.loadSession("s1");
    QVERIFY(copy.has_value());
    QCOMPARE(copy->calculationEngine().request(kCalcX).status, ResultStatus::Ok);
    const std::optional<StoredCalculationResult> exported = copy->calculationEngine().exportResult(kCalcX);
    QVERIFY(exported.has_value());

    logbook.setCachedValues("s1", {{xColumn(), QStringLiteral("v")}, {descriptionColumn(), QStringLiteral("d")}});
    QSignalSpy spy(&logbook, &LogbookManager::calculationRecordsChanged);
    QVERIFY(logbook.writeCalculationRecord("s1", CalculationRecord::stamped(*exported)));

    QCOMPARE(logbook.cachedValuesForSession("s1").keys(),
             QStringList({LogbookManager::columnDefKey(descriptionColumn())}));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("s1"));
    QCOMPARE(spy.at(0).at(1).toString(), kCalcX);
    QCOMPARE(logbook.knownCalculationRecords("s1"), QSet<QString>({kCalcX}));

    // Absent and unknown: nothing changed, nothing is said
    QVERIFY(logbook.removeCalculationRecord("s1", kCalcY));
    QCOMPARE(spy.count(), 1);
}

// The main window's delete sequence: the entry goes with its stamp; the other
// session keeps its values and stamp.
void ResultColumnsTest::deletingSessionRemovesStamp()
{
    QCOMPARE(engine("s1").request(kCalcX).status, ResultStatus::Ok);
    QVERIFY(fit("s2", kCalcY));
    const QJsonObject before = readIndex();
    const QJsonValue s2Values = before[QStringLiteral("sessions")].toObject()["s2"].toObject()["values"];
    const QJsonValue s2Stamp = indexRecordStamp(before, "s2");
    QCOMPARE(s2Stamp, stampOf({{kCalcY, QStringLiteral("y-v1")}}));
    const QString path = recordPath("s1", kEncodedX);
    QVERIFY(QFileInfo(path).isFile());

    LogbookManager &logbook = LogbookManager::instance();
    QVERIFY(m_model->removeSessions({"s1"}));
    QVERIFY(logbook.removeSession("s1"));
    QVERIFY(logbook.flushIndex());

    const QJsonObject after = readIndex();
    QVERIFY(!after[QStringLiteral("sessions")].toObject().contains(QStringLiteral("s1")));
    QVERIFY(!QFileInfo::exists(path));
    QCOMPARE(after[QStringLiteral("sessions")].toObject()["s2"].toObject()["values"].toObject().size(),
             s2Values.toObject().size());
    QCOMPARE(indexValue(after, "s2", yColumn()), QJsonValue(10.0));
    QCOMPARE(indexValue(after, "s2", descriptionColumn()), QJsonValue(QStringLiteral("d2")));
    QCOMPARE(indexRecordStamp(after, "s2"), s2Stamp);
}

// A session reserved at import and never saved: removeSession() forgets
// everything kept for its id, as for a saved session, and its records go.
void ResultColumnsTest::removingReservedSessionForgetsIt()
{
    QCOMPARE(m_model->mergeSessions({columnSession(QStringLiteral("n1"), QStringLiteral("dn"), 3.0)})
                 .at(0).outcome, MergeResult::Outcome::Created);
    QCOMPARE(engine("n1").request(kCalcY).status, ResultStatus::Ok);
    LogbookManager &logbook = LogbookManager::instance();
    logbook.setLastAccessed(QStringLiteral("n1"), 1727000000.0);
    logbook.setCachedValues(QStringLiteral("n1"), {{descriptionColumn(), QStringLiteral("dn")}});
    QVERIFY(logbook.hasUnsavedColumns("n1"));
    QCOMPARE(logbook.knownCalculationRecords("n1"), QSet<QString>({kCalcY}));
    const QStringList recordsBefore = calculationRecordFiles();
    QCOMPARE(recordsBefore.size(), 1);

    // The main window's delete sequence, before the idle saver ran
    QVERIFY(m_model->removeSessions({"n1"}));
    QVERIFY(logbook.removeSession("n1"));

    QVERIFY(!logbook.lastAccessedMap().contains(QStringLiteral("n1")));
    QVERIFY(logbook.cachedValuesForSession("n1").isEmpty());
    QVERIFY(!logbook.hasUnsavedColumns("n1"));
    QCOMPARE(logbook.knownCalculationRecords("n1"), QSet<QString>());
    QCOMPARE(logbook.unconfirmedCalculationRecords("n1"), QSet<QString>());
    QCOMPARE(calculationRecordFiles(), QStringList());
    QVERIFY(!logbook.removeSession("n1"));
}

// Everything in cache/ is derived: deleting the folder while the application
// is closed is safe. At the next start no record is known, every requested
// calculation reads as not requested, and the start-up stamp check drops each
// cached value over a vanished record (the rest of the index is kept). Nothing
// runs, no job is created, no record is read and none is written again; the
// column worker settles the values as unavailable without a load.
void ResultColumnsTest::deletedCacheFolderForgetsRequests()
{
    TestEnvironment &env = TestEnvironment::instance();
    LogbookManager &logbook = LogbookManager::instance();

    QVERIFY(fit("s1", kCalcX));
    QVERIFY(fit("s1", kCalcY));
    QVERIFY(fit("s2", kCalcY));
    QCOMPARE(calculationRecordFiles().size(), 3);
    QCOMPARE(indexValue("s1", xColumn()), QJsonValue(QStringLiteral("x:d1")));
    QCOMPARE(indexValue("s1", yColumn()), QJsonValue(6.0));
    QCOMPARE(indexValue("s2", yColumn()), QJsonValue(10.0));
    QCOMPARE(indexRecordStamp("s1"), stampOf({{kCalcX, QString()}, {kCalcY, QStringLiteral("y-v1")}}));
    QCOMPARE(indexRecordStamp("s2"), stampOf({{kCalcY, QStringLiteral("y-v1")}}));
    const QStringList sessionFiles = sessionCsvFiles();
    const QByteArray csv1 = bytesOf(sessionFilePath("s1"));
    const QByteArray csv2 = bytesOf(sessionFilePath("s2"));

    // The application closes; the user deletes cache/; the application starts
    resetModel();
    QVERIFY(QDir(env.cacheDir()).removeRecursively());
    QVERIFY(!QFileInfo::exists(env.cacheDir()));
    restart();

    // No record is known, and exactly the values over a vanished record are
    // gone: X and Y of s1, Y of s2. X of s2 (never requested, cached as
    // unavailable with no record) and the descriptions stay.
    const QString xKey = LogbookManager::columnDefKey(xColumn());
    const QString yKey = LogbookManager::columnDefKey(yColumn());
    const QString dKey = LogbookManager::columnDefKey(descriptionColumn());
    QVERIFY(!logbook.cachedValuesDiscardedOnLoad());
    for (const char *id : {"s1", "s2"}) {
        QVERIFY(logbook.knownCalculationRecords(id).isEmpty());
        QVERIFY(logbook.calculationRecordIds(id).isEmpty());
        QVERIFY(!logbook.cachedValuesForSession(id).contains(yKey));
        QVERIFY(logbook.cachedValuesForSession(id).contains(dKey));
        QVERIFY(!isLoaded(id));
        QVERIFY(!isCached(id, kY));
    }
    QVERIFY(!logbook.cachedValuesForSession("s1").contains(xKey));
    QVERIFY(!isCached("s1", kX));
    QVERIFY(logbook.cachedValuesForSession("s2").value(xKey).isNull());
    QVERIFY(isCached("s2", kX));
    QVERIFY(!cached("s2", kX).isValid());
    QCOMPARE(cached("s1", kD), QVariant(QStringLiteral("d1")));
    QCOMPARE(cached("s2", kD), QVariant(QStringLiteral("d2")));

    // The column worker settles them as unavailable without loading anything
    m_model->resetColumnWorkStats();
    m_model->resetStoredResultStats();
    m_model->startColumnWorker();
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, 0);
    for (const char *id : {"s1", "s2"}) {
        QVERIFY(!isLoaded(id));
        for (int column : {kX, kY}) {
            QVERIFY(isCached(id, column));
            QVERIFY(!cached(id, column).isValid());
        }
        QVERIFY(indexValue(id, xColumn()).isNull());
        QVERIFY(indexValue(id, yColumn()).isNull());
        QCOMPARE(indexRecordStamp(id), QJsonValue(QJsonObject()));
    }

    // Loading a session: not requested, nothing restored, nothing run
    for (const char *id : {"s1", "s2"}) {
        session(id);
        QVERIFY(waitForIdle(*m_model));
        for (const QString &calculation : {kCalcX, kCalcY}) {
            QCOMPARE(engine(id).resultStatus(calculation), std::optional<ResultStatus>());
            QCOMPARE(engine(id).runCount(calculation), 0);
        }
        QVERIFY(!session(id).getAttribute(QStringLiteral("Y_OUT")).isValid());
        QVERIFY(!session(id).getAttribute(QStringLiteral("X_OUT")).isValid());
        QVERIFY(!cached(id, kY).isValid());
    }
    QCOMPARE(stats().restoreCalls, 2);
    QCOMPARE(stats().recordListings, 0);
    QCOMPARE(stats().recordsRead, 0);
    QCOMPARE(stats().recordsWritten, 0);
    QCOMPARE(m_queue->model()->rowCount(), 0);

    // No record came back, and the recordings are untouched
    QVERIFY(!QFileInfo::exists(env.cacheDir()));
    QCOMPARE(sessionCsvFiles(), sessionFiles);
    QCOMPARE(bytesOf(sessionFilePath("s1")), csv1);
    QCOMPARE(bytesOf(sessionFilePath("s2")), csv2);
}

// ---- Column demand ---------------------------------------------------------------------

void ResultColumnsTest::columnWorkerIsUnchangedByDemand_data()
{
    QTest::addColumn<bool>("withDemand");
    QTest::newRow("without demand") << false;
    QTest::newRow("with demand") << true;
}

// The workerRestoresStoredResult set-up (s1 has an X record and its X value is
// gone from the index; s2 has none), with and without a demand layer: the
// column worker settles and computes exactly the same, counts the same, and
// the demand layer's loads come after its pass. With demand, the missing
// result of s2 is computed afterwards; the record of s1 counts as its result.
void ResultColumnsTest::columnWorkerIsUnchangedByDemand()
{
    QFETCH(bool, withDemand);
    LogbookColumnStore::instance().setColumns({descriptionColumn(), xColumn()});     // cleanup() restores

    QCOMPARE(engine("s1").request(kCalcX).status, ResultStatus::Ok);
    QVERIFY(waitForIdle(*m_model));
    resetModel();
    QVERIFY(editIndex([](QJsonObject &root) { removeIndexValue(root, "s1", xColumn()); }));
    restart();
    QVERIFY(!isCached("s1", kX));
    QVERIFY(isCached("s2", kX));
    QVERIFY(!cached("s2", kX).isValid());

    m_model->resetColumnWorkStats();
    m_model->resetStoredResultStats();
    QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);

    // Destroyed before cleanup() resets the queue: demand, then plots
    PlotModel plots;
    std::unique_ptr<CalculationDemand> demand;
    if (withDemand)
        demand = std::make_unique<CalculationDemand>(m_model.get(), &plots, m_queue.get());

    struct Snapshot {
        SessionModel::ColumnWorkStats work;
        CalculationResultStore::Stats store;
        QVariant s1X;
        bool s2XCachedUnavailable = false;
        int loads = 0;
        bool taken = false;
    } snapshot;
    const auto take = [&] {
        if (snapshot.taken)
            return;
        snapshot.taken = true;
        snapshot.work = m_model->columnWorkStats();
        snapshot.store = stats();
        snapshot.s1X = cached("s1", kX);
        snapshot.s2XCachedUnavailable = isCached("s2", kX) && !cached("s2", kX).isValid();
        snapshot.loads = int(loadedSpy.count());
    };
    QObject scope;
    connect(&m_model->scheduler(), &IdleScheduler::activeTaskChanged, &scope, [&take](int id, bool) {
        if (id == SessionModel::ColumnFillTask)
            take();
    });

    m_model->startColumnWorker();
    if (withDemand) {
        QVERIFY(waitDemandIdle(*m_queue, *demand, 10000));
        QVERIFY(waitForIdle(*m_model));
    } else {
        QVERIFY(waitForIdle(*m_model));
        take();
    }
    QVERIFY(snapshot.taken);

    QCOMPARE(snapshot.work.sessionsLoaded, 1);
    QCOMPARE(snapshot.work.valuesComputed, 1);
    QCOMPARE(snapshot.work.calculationRuns, 0);
    QCOMPARE(snapshot.store.restoreCalls, 1);
    QCOMPARE(snapshot.store.recordsRead, 1);
    QCOMPARE(snapshot.store.recordsRestored, 1);
    QCOMPARE(snapshot.store.staleRecordsDeleted, 0);
    QCOMPARE(snapshot.store.recordsWritten, 0);
    QCOMPARE(snapshot.s1X, QVariant(QStringLiteral("x:d1")));
    QVERIFY(snapshot.s2XCachedUnavailable);
    QCOMPARE(snapshot.loads, 0);

    if (!withDemand) {
        QCOMPARE(loadedSpy.count(), 0);
        QCOMPARE(m_queue->model()->rowCount(), 0);
        return;
    }

    // Afterwards: s2 loaded once, computed, stored and stamped; s1 never loaded
    QCOMPARE(loadedSpy.count(), 1);
    QCOMPARE(loadedSpy.at(0).at(0).toString(), QStringLiteral("s2"));
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(m_queue->model()->record(0).sessionId, QStringLiteral("s2"));
    QCOMPARE(m_queue->model()->record(0).state, JobState::Succeeded);
    QCOMPARE(cached("s2", kX), QVariant(QStringLiteral("x:d2")));
    QVERIFY(LogbookManager::instance().knownCalculationRecords("s2").contains(kCalcX));
    m_model->flushDirtySessions();
    QCOMPARE(indexValue("s2", xColumn()), QJsonValue(QStringLiteral("x:d2")));
    QCOMPARE(indexRecordStamp("s2"), stampOf({{kCalcX, QString()}}));
    QVERIFY(!isLoaded("s1"));
    QCOMPARE(cached("s1", kX), QVariant(QStringLiteral("x:d1")));
    QVERIFY(demand->columnState(CalculationDemand::columnId(xColumn())).isPlain());
}

// A bulk edit on a stub leaves its X record stale. Until the column worker's
// copy finds it so, a known record counts as a result and nothing is in
// demand; its deletion moves the pair into demand, the column fill loads the
// session once, and one job writes the record again.
void ResultColumnsTest::staleRecordDeletedByWorkerCreatesDemand()
{
    LogbookColumnStore::instance().setColumns({descriptionColumn(), xColumn()});     // cleanup() restores
    QVERIFY(fit("s1", kCalcX));
    QVERIFY(fit("s2", kCalcX));
    QCOMPARE(evict({"s1"}), QString());
    const QString path = recordPath("s1", kEncodedX);
    QVERIFY(QFileInfo(path).isFile());

    PlotModel plots;
    const auto demand = std::make_unique<CalculationDemand>(m_model.get(), &plots, m_queue.get());
    const QString x = CalculationDemand::columnId(xColumn());
    {
        const Quiet quiet(*m_queue);
        demand->flush();
        QVERIFY(demand->columnState(x).isPlain());
        QCOMPARE(demand->columnState(x).doneCount, 2);
        PlotFixture::spin(demand.get());
        QVERIFY(quiet.holds());
    }

    m_model->resetStoredResultStats();
    QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);
    QStringList sequence;
    bool pendingBeforeWorker = true;
    bool workerSeen = false;
    QObject scope;
    connect(&m_model->scheduler(), &IdleScheduler::activeTaskChanged, &scope, [&](int id, bool) {
        if (id != SessionModel::ColumnTask || workerSeen)
            return;
        workerSeen = true;          // before the first step of the worker
        demand->flush();
        pendingBeforeWorker = demand->isCellPending(QStringLiteral("s1"), x);
    });
    connect(&LogbookManager::instance(), &LogbookManager::calculationRecordsChanged, &scope,
            [&](const QString &id, const QString &calculationId) {
        if (id == QLatin1String("s1") && calculationId == kCalcX)
            sequence.append(QStringLiteral("changed:loads=%1").arg(loadedSpy.count()));
    });
    connect(demand.get(), &CalculationDemand::columnStateChanged, &scope, [&](const QString &) {
        if (demand->isCellPending(QStringLiteral("s1"), x) && !sequence.contains(QStringLiteral("pending")))
            sequence.append(QStringLiteral("pending"));
    });
    connect(m_model.get(), &SessionModel::sessionLoaded, &scope,
            [&](const QString &id) { sequence.append(QStringLiteral("loaded:") + id); });
    connect(m_queue.get(), &JobQueue::jobStarted, &scope,
            [&](JobId job) { sequence.append(QStringLiteral("job:") + m_queue->job(job).sessionId); });

    m_model->startBulkEdit({row("s1")}, kD, QStringLiteral("bulk"));
    QVERIFY(waitDemandIdle(*m_queue, *demand, 10000));
    QVERIFY(waitForIdle(*m_model));

    QVERIFY(workerSeen);
    QVERIFY(!pendingBeforeWorker);      // a known record counts as a result
    QCOMPARE(sequence.value(0), QStringLiteral("changed:loads=0"));   // the worker's deletion, before any load
    QCOMPARE(stats().staleRecordsDeleted, 1);
    const qsizetype pending = sequence.indexOf(QStringLiteral("pending"));
    const qsizetype loaded = sequence.indexOf(QStringLiteral("loaded:s1"));
    const qsizetype job = sequence.indexOf(QStringLiteral("job:s1"));
    QVERIFY2(pending > 0 && loaded > pending && job > loaded, qPrintable(sequence.join(QLatin1Char(' '))));
    QCOMPARE(loadedSpy.count(), 1);
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(m_queue->model()->record(0).calculationId, kCalcX);
    QCOMPARE(m_queue->model()->record(0).sessionId, QStringLiteral("s1"));
    QCOMPARE(m_queue->model()->record(0).state, JobState::Succeeded);
    QVERIFY(QFileInfo(path).isFile());
    QCOMPARE(stats().recordsWritten, 1);
    QCOMPARE(cached("s1", kX), QVariant(QStringLiteral("x:bulk")));
    m_model->flushDirtySessions();
    QCOMPARE(indexValue("s1", xColumn()), QJsonValue(QStringLiteral("x:bulk")));
    QCOMPARE(indexRecordStamp("s1"), stampOf({{kCalcX, QString()}}));
    QVERIFY(demand->columnState(x).isPlain());
}

FLYSIGHT_TEST_MAIN(ResultColumnsTest)
#include "tst_result_columns.moc"
