// Stored results of explicit calculations on a real SessionModel, a real
// logbook and a real JobQueue, with fast synthetic calculations
// (store-requested-calculations):
//
//  - an Ok install writes a record, on both install paths (a job's publish and
//    a synchronous request), also for a session that an import has just
//    created and that has not been saved yet (its reserved file stem);
//  - an install with any other status writes and deletes nothing;
//  - a write failure (a directory at the record's path, a type the record
//    format refuses) leaves the in-memory result usable and the previous
//    record intact;
//  - an input change deletes the record; eviction and a restart do not; a
//    registry change made while the application runs that changes what a
//    name a result looked up resolves to deletes its record, one behind the
//    provider and a teardown removal do not; a registry or
//    preference change that does not reach it leaves the record valid across
//    loads and restarts; a record whose lookups resolve differently at load,
//    or whose plug-in provider's code identity changed, is stale; an
//    unreadable record is skipped (and every record that reads it), restored
//    later, replaced by the next publish, and left behind or removed with its
//    session as documented; a format-1 record is deleted;
//  - an explicit family instance is not stored (its install writes nothing,
//    its drop removes nothing);
//  - every load path that installs a session into a row restores its valid
//    records before the row is published, upstream records first
//    (explicit-on-explicit chains, a fallback candidate included), and deletes
//    the stale ones; a result already installed wins; a
//    session the logbook knows no record of is not listed;
//  - the column worker's and the bulk edit's temporary loads never read one;
//  - deleting a session removes its records, and a stray is removed at the
//    next start.
//
// Expected values are literals (EA1 == 5, "negative input", file names),
// never recomputed with the code under test. Only the unreadable rows use a
// platform mechanism (a lock on Windows, permission bits elsewhere) and skip
// where it is not honoured. Nothing depends on case sensitivity or on
// directory iteration order.

#include <functional>
#include <memory>
#include <optional>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QtTest>

#include "calculationrecord.h"
#include "calculationresultstore.h"
#include "calculations/builtincalculations.h"
#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "engine/storedcalculationresult.h"
#include "jobfixture.h"
#include "jobmodel.h"
#include "jobqueue.h"
#include "logbookcolumn.h"
#include "logbookmanager.h"
#include "logbookprobe.h"
#include "plugincodeidentity.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "sessiondata.h"
#include "sessionmodel.h"
#include "storedresults.h"
#include "testenvironment.h"
#include "testmain.h"
#include "testutil.h"

using namespace FlySight;
using namespace FlySightTest;

using Kind = JobQueue::RequestResult::Kind;

Q_DECLARE_METATYPE(FlySight::DependencyKey)

namespace {

const QString kExpA = QStringLiteral("expA");
const QString kExpB = QStringLiteral("expB");
const QString kThrower = QStringLiteral("thrower");
const QString kUp = QStringLiteral("test.store.up");
const QString kDown = QStringLiteral("test.store.down");
const QString kListy = QStringLiteral("test.store.listy");
const QString kShadow = QStringLiteral("test.store.shadow");
const QString kExtra = QStringLiteral("test.store.extra");
const QString kGone = QStringLiteral("test.store.gone");
const QString kFamily = QStringLiteral("test.store.fam");
const QString kFamilySrc = QStringLiteral("test.store.famSrc");
const QString kFromSrc = QStringLiteral("test.store.fromSrc");
const QString kSrc = QStringLiteral("test.store.src");
const QString kBehind = QStringLiteral("test.store.behind");
const QString kReader = QStringLiteral("test.store.reader");
const QString kPlugin = QStringLiteral("test.store.plugin");
const QString kReadsPlugin = QStringLiteral("test.store.readsPlugin");
const QString kReadsNoPlugin = QStringLiteral("test.store.readsNoPlugin");
const QString kSkipped = QStringLiteral("skipped (kept for the next load)");

DependencyKey attrKey(const char *key)
{
    return DependencyKey::attribute(QString::fromLatin1(key));
}

/// Explicit calculations of this test, on the GLOBAL registry (the one real
/// sessions are bound to); the destructor unregisters exactly these ids.
/// Construct after JobWorld and before the SessionModel; destroy after the
/// JobQueue and the SessionModel are gone (as JobWorld).
///
/// | Id               | Policy   | Inputs                   | Output                                                   |
/// |------------------|----------|--------------------------|----------------------------------------------------------|
/// | test.store.up    | Explicit | attr UP_IN               | UP_OUT = UP_IN * 3 (int)                                 |
/// | test.store.down  | Explicit | attr UP_OUT, attr DOWN_IN| DOWN_OUT = UP_OUT + DOWN_IN (int)                        |
/// | test.store.listy | Explicit | attr LY_IN               | LY_OUT = LY_IN * 2 (int) when LY_IN >= 0; else           |
/// |                  |          |                          | QVariantList{LY_IN} (a type records refuse)              |
///
/// Literals: UP_IN = 2, DOWN_IN = 5 give UP_OUT 6 and DOWN_OUT 11.
/// "test.store.down" sorts before "test.store.up": a restore in id order
/// meets the downstream record first.
class StoreWorld {
public:
    StoreWorld()
    {
        CalculationRegistry &registry = CalculationRegistry::instance();

        CalculationDescriptor up;
        up.id = kUp;
        up.policy = EvaluationPolicy::Explicit;
        up.inputs = {CalcInput::attribute(QStringLiteral("UP_IN"))};
        up.outputs = {attrKey("UP_OUT")};
        up.compute = [](const EvaluationContext &ctx) {
            return CalculationResult().setAttribute(QStringLiteral("UP_OUT"),
                                                    ctx.attribute(QStringLiteral("UP_IN")).toInt() * 3);
        };

        CalculationDescriptor down;
        down.id = kDown;
        down.policy = EvaluationPolicy::Explicit;
        down.inputs = {CalcInput::attribute(QStringLiteral("UP_OUT")), CalcInput::attribute(QStringLiteral("DOWN_IN"))};
        down.outputs = {attrKey("DOWN_OUT")};
        down.compute = [](const EvaluationContext &ctx) {
            return CalculationResult().setAttribute(QStringLiteral("DOWN_OUT"),
                                                    ctx.attribute(QStringLiteral("UP_OUT")).toInt()
                                                        + ctx.attribute(QStringLiteral("DOWN_IN")).toInt());
        };

        CalculationDescriptor listy;
        listy.id = kListy;
        listy.policy = EvaluationPolicy::Explicit;
        listy.inputs = {CalcInput::attribute(QStringLiteral("LY_IN"))};
        listy.outputs = {attrKey("LY_OUT")};
        listy.compute = [](const EvaluationContext &ctx) {
            const int in = ctx.attribute(QStringLiteral("LY_IN")).toInt();
            if (in >= 0)
                return CalculationResult().setAttribute(QStringLiteral("LY_OUT"), in * 2);
            return CalculationResult().setAttribute(QStringLiteral("LY_OUT"), QVariantList{QVariant(in)});
        };

        for (const CalculationDescriptor &d : {up, down, listy}) {
            if (registry.registerCalculation(d))
                m_ids.append(d.id);
        }
    }
    ~StoreWorld()
    {
        for (const QString &id : std::as_const(m_ids))
            CalculationRegistry::instance().unregister(id, CalculationRegistry::Removal::Change);
    }
    StoreWorld(const StoreWorld &) = delete;
    StoreWorld &operator=(const StoreWorld &) = delete;

    QStringList registeredIds() const { return m_ids; }

private:
    QStringList m_ids;
};

/// An OnDemand calculation with no inputs and one int attribute output.
CalculationDescriptor constantCalculation(const QString &id, const QString &output, int value)
{
    CalculationDescriptor d;
    d.id = id;
    d.outputs = {DependencyKey::attribute(output)};
    d.compute = [output, value](const EvaluationContext &) {
        return CalculationResult().setAttribute(output, value);
    };
    return d;
}

/// The bytes of a file; a null QByteArray when it does not exist or is not a file.
QByteArray bytesOf(const QString &path)
{
    QFile file(path);
    if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(bytes) == bytes.size();
}

/// An OnDemand calculation `output` = `input` + 1 (int).
CalculationDescriptor plusOne(const QString &id, const QString &input, const QString &output)
{
    CalculationDescriptor d;
    d.id = id;
    d.inputs = {CalcInput::attribute(input)};
    d.outputs = {DependencyKey::attribute(output)};
    d.compute = [input, output](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute(output, ctx.attribute(input).toInt() + 1);
    };
    return d;
}

/// An Explicit calculation `output` = `input` * `factor` (int).
CalculationDescriptor timesExplicit(const QString &id, const QString &input, const QString &output, int factor)
{
    CalculationDescriptor d;
    d.id = id;
    d.policy = EvaluationPolicy::Explicit;
    d.inputs = {CalcInput::attribute(input)};
    d.outputs = {DependencyKey::attribute(output)};
    d.compute = [input, output, factor](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute(output, ctx.attribute(input).toInt() * factor);
    };
    return d;
}

/// True when `resolutions` holds exactly this entry (a null and an empty
/// string compare equal).
bool hasResolution(const QList<StoredResolution> &resolutions, const DependencyKey &name,
                   StoredResolution::Provider provider, const QString &instanceId = QString(),
                   const QString &resultVersion = QString())
{
    for (const StoredResolution &r : resolutions) {
        if (r.name == name && r.provider == provider && r.instanceId == instanceId
            && r.resultVersion == resultVersion)
            return true;
    }
    return false;
}

/// The plug-in ingredients of pluginEditStalesRecordsThatReadIt(): "v1" (the
/// stand-in folder), or v1 with the one change a row names.
PluginCodeIngredients pluginIngredients(const QString &row)
{
    PluginCodeIngredients i = standInPluginIngredients();
    if (row == QLatin1String("editedFile"))
        i.files[0].bytes = QByteArray("x = 2\n");
    else if (row == QLatin1String("addedFile"))
        i.files.append(PluginSourceFile{QStringLiteral("b_plugin.py"), QByteArray("z = 3\n")});
    else if (row == QLatin1String("pythonVersion"))
        i.pythonVersion = QStringLiteral("3.13.4");
    else if (row == QLatin1String("numpyVersion"))
        i.numpyVersion = QStringLiteral("2.2.5");
    return i;
}

/// What a sessionLoaded watcher saw for one session.
struct LoadWatch {
    bool seen = false;
    std::optional<ResultStatus> status;     ///< of the watched calculation, at the emission
};

} // namespace

class ResultStoreTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void writesOnOkInstall_data();
    void writesOnOkInstall();
    void rejectionIsWritten();
    void nonOkInstallWritesAndDeletesNothing();
    void recordBeforeFirstSave_data();
    void recordBeforeFirstSave();
    void recordOfNeverSavedSessionIsStray();
    void removeBeforeFirstSave();
    void writeFailureLeavesResultUsable();
    void writeFailureKeepsPreviousRecord();
    void inputChangeDeletesRecord();
    void noDeleteWithoutInputChange();
    void explicitFamilyIsNotStored();
    void restoreOnEveryLoadPath_data();
    void restoreOnEveryLoadPath();
    void bulkEditPromotionRestores();
    void restoresChainInPasses();
    void upstreamMissingAfterLastPassDeletes();
    void staleRecordDeletedOnLoad_data();
    void staleRecordDeletedOnLoad();
    void formatOneRecordIsDeletedOnLoad();
    void unreadableRecordIsSkipped_data();
    void unreadableRecordIsSkipped();
    void dependentOfSkippedRecordIsKept();
    void restoresUpstreamFirst_data();
    void restoresUpstreamFirst();
    void skippedRecordIsReplacedByPublish_data();
    void skippedRecordIsReplacedByPublish();
    void deletingSessionWithSkippedRecord_data();
    void deletingSessionWithSkippedRecord();
    void registryChangeDeletesRecord_data();
    void registryChangeDeletesRecord();
    void recordSurvivesUnrelatedChanges_data();
    void recordSurvivesUnrelatedChanges();
    void lookupResolvingDifferentlyDeletesRecord();
    void pluginEditStalesRecordsThatReadIt_data();
    void pluginEditStalesRecordsThatReadIt();
    void alreadyInstalledIsKept();
    void temporaryLoadsNeverRestore();
    void deletingSessionRemovesRecords();
    void strayRecordRemovedAtRestart();

private:
    SessionData &session(const QString &id) { return m_model->sessionRef(m_model->getSessionRow(id)); }
    CalculationEngine &engine(const QString &id) { return session(id).calculationEngine(); }
    int row(const QString &id) const { return m_model->getSessionRow(id); }
    bool isLoaded(const QString &id) const
    {
        const int r = row(id);
        return r >= 0 && std::as_const(*m_model).rowAt(r).isLoaded();
    }
    /// The application's edit path. A test function checks it with QVERIFY.
    [[nodiscard]] bool setInput(const QString &id, const char *key, int value)
    {
        return m_model->updateAttribute(id, QString::fromLatin1(key), value);
    }
    const CalculationResultStore::Stats &stats() const { return m_model->storedResultStats(); }

    /// cache/<stem of id>.<encodedId>.fvresult; the stem comes from
    /// index.json on disk, so the session must be saved and the index flushed.
    static QString recordPath(const QString &id, const QString &encodedId)
    {
        return TestEnvironment::instance().cacheDir() + QLatin1Char('/') + sessionFileStem(id)
            + QLatin1Char('.') + encodedId + QStringLiteral(".fvresult");
    }

    /// Makes each row a stub: touches it (a created row is not in the LRU),
    /// then evicts with capacity 0 and sets the capacity back to 50. Empty
    /// when every row is a stub afterwards.
    [[nodiscard]] QString evict(const QStringList &ids);
    /// A simulated application restart: new logbook state, a new model of
    /// stubs from the index, a new queue. `whileClosed` runs after the old
    /// model is gone and before initialize().
    void restart(const std::function<void()> &whileClosed = {});
    /// The record of (id, calculationId) as read now; nullopt unless Ok.
    static std::optional<CalculationRecord> storedRecord(const QString &id, const QString &calculationId)
    {
        const CalculationRecordRead read = LogbookManager::instance().readCalculationRecord(id, calculationId);
        return read.status == CalculationRecordStatus::Ok ? read.record : std::nullopt;
    }
    /// Reads the record, applies `mutate`, writes it back. Empty on success.
    [[nodiscard]] QString rewriteRecord(const QString &id, const QString &calculationId,
                                        const std::function<void(CalculationRecord &)> &mutate);
    /// Notes the status of `calculationId` in session `id` at every sessionLoaded for it.
    void watchLoad(QObject *scope, const QString &id, const QString &calculationId, LoadWatch *out);

    std::unique_ptr<JobWorld> m_world;
    std::unique_ptr<StoreWorld> m_store;
    std::unique_ptr<ExtraRegistrations> m_extra;
    std::unique_ptr<SessionModel> m_model;
    std::unique_ptr<JobQueue> m_queue;
    QStringList m_registryBefore;
};

void ResultStoreTest::initTestCase()
{
    TestEnvironment::instance().registerBuiltIns();

    // One logbook column that reads stored data only
    PreferencesManager::instance().registerPreference(PreferenceKeys::LogbookColumnsVersion, 0);
    LogbookColumnStore::instance().setColumns({descriptionColumn()});

    qRegisterMetaType<DependencyKey>();
}

// Four loaded, hidden, unfocused, saved sessions s1..s4 from the descent
// fixture (so every one has a file stem), none with an input of any synthetic
// calculation.
void ResultStoreTest::init()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();
    env.resetPreferencesToDefaults();
    LogbookManager::instance().initialize();
    m_registryBefore = CalculationRegistry::instance().registeredIds();

    m_world = std::make_unique<JobWorld>();
    m_store = std::make_unique<StoreWorld>();
    QCOMPARE(m_store->registeredIds(), QStringList({kUp, kDown, kListy}));
    m_extra = std::make_unique<ExtraRegistrations>();

    m_model = std::make_unique<SessionModel>();
    m_model->mergeSessions(JobWorld::sessions({"s1", "s2", "s3", "s4"}));
    QCOMPARE(m_model->rowCount(), 4);
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(!sessionFileStem("s1").isEmpty());

    m_queue = std::make_unique<JobQueue>(m_model.get());
}

// Note nothing, tear everything down, and only then check (see tst_jobqueue).
void ResultStoreTest::cleanup()
{
    if (m_queue)
        m_queue->shutdown();
    m_queue.reset();
    m_model.reset();
    m_extra.reset();
    m_store.reset();
    m_world.reset();

    LogbookColumnStore::instance().setColumns({descriptionColumn()});
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);

    QCOMPARE(CalculationRegistry::instance().registeredIds(), m_registryBefore);
    QCOMPARE(CalculationRegistry::instance().enrolledEngineCount(), 0);
}

QString ResultStoreTest::evict(const QStringList &ids)
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

void ResultStoreTest::restart(const std::function<void()> &whileClosed)
{
    if (m_queue)
        m_queue->shutdown();
    m_queue.reset();
    m_model.reset();
    if (whileClosed)
        whileClosed();

    LogbookManager &logbook = LogbookManager::instance();
    TestEnvironment::instance().reopenLogbook();
    logbook.initialize();

    m_model = std::make_unique<SessionModel>();
    m_model->populateFromIndex(logbook.cachedColumnValues(LogbookColumnStore::instance().enabledColumns()),
                               logbook.lastAccessedMap());
    m_queue = std::make_unique<JobQueue>(m_model.get());
}

QString ResultStoreTest::rewriteRecord(const QString &id, const QString &calculationId,
                                       const std::function<void(CalculationRecord &)> &mutate)
{
    LogbookManager &logbook = LogbookManager::instance();
    const CalculationRecordRead read = logbook.readCalculationRecord(id, calculationId);
    if (read.status != CalculationRecordStatus::Ok)
        return QStringLiteral("the record could not be read: ") + read.error;
    CalculationRecord record = *read.record;
    mutate(record);
    QString error;
    if (!logbook.writeCalculationRecord(id, record, &error))
        return QStringLiteral("the record could not be written: ") + error;
    return QString();
}

void ResultStoreTest::watchLoad(QObject *scope, const QString &id, const QString &calculationId, LoadWatch *out)
{
    SessionModel *model = m_model.get();
    connect(model, &SessionModel::sessionLoaded, scope, [model, id, calculationId, out](const QString &loadedId) {
        if (loadedId != id)
            return;
        const auto guard = model->stableRows();
        const SessionData *loaded = model->loadedSession(id);
        out->seen = true;
        out->status = loaded ? loaded->calculationEngine().resultStatus(calculationId) : std::nullopt;
    });
}

// ---- Writing -------------------------------------------------------------------------

void ResultStoreTest::writesOnOkInstall_data()
{
    QTest::addColumn<bool>("queued");
    QTest::newRow("request") << false;
    QTest::newRow("queue") << true;
}

// An Ok install writes exactly one record, stamped and equal to the export,
// before control returns to the event loop.
void ResultStoreTest::writesOnOkInstall()
{
    QFETCH(bool, queued);
    QVERIFY(setInput("s1", "EA_IN", 4));
    QVERIFY(waitForIdle(*m_model));
    const QString stem = sessionFileStem("s1");
    const QString path = recordPath("s1", QStringLiteral("exp%41"));
    QVERIFY(!QFileInfo::exists(path));
    m_model->resetStoredResultStats();

    bool existedAtInstall = false;
    QObject scope;      // owns the connection
    if (queued) {
        connect(m_queue.get(), &JobQueue::jobFinished, &scope, [&existedAtInstall, path](JobId, JobState) {
            existedAtInstall = QFileInfo(path).isFile();
        });
        QCOMPARE(m_queue->request("s1", kExpA).kind, Kind::Created);
        QVERIFY(waitIdle(*m_queue));
        QCOMPARE(m_queue->model()->record(0).state, JobState::Succeeded);
    } else {
        QCOMPARE(engine("s1").request(kExpA).status, ResultStatus::Ok);
        existedAtInstall = QFileInfo(path).isFile();
    }
    QVERIFY(existedAtInstall);

    QCOMPARE(calculationRecordFiles(), QStringList({stem + QStringLiteral(".exp%41.fvresult")}));
    const CalculationRecordRead read = LogbookManager::instance().readCalculationRecord("s1", kExpA);
    QCOMPARE(read.status, CalculationRecordStatus::Ok);
    QVERIFY(read.record->stampsAreCurrent());
    const std::optional<StoredCalculationResult> exported = engine("s1").exportResult(kExpA);
    QVERIFY(exported.has_value());
    QVERIFY(sameContent(read.record->result, *exported));
    QCOMPARE(stats().recordsWritten, 1);
    QCOMPARE(stats().writeFailures, 0);
}

// A rejection is an Ok result with a reason: it is stored like any other.
void ResultStoreTest::rejectionIsWritten()
{
    QVERIFY(setInput("s1", "EA_IN", -1));
    QCOMPARE(engine("s1").request(kExpA).status, ResultStatus::Ok);

    const CalculationRecordRead read = LogbookManager::instance().readCalculationRecord("s1", kExpA);
    QCOMPARE(read.status, CalculationRecordStatus::Ok);
    const StoredCalculationResult &result = read.record->result;
    QCOMPARE(result.detail, QStringLiteral("negative input"));
    QCOMPARE(result.bundle.reason(), QStringLiteral("negative input"));
    QCOMPARE(result.bundle.attributeValue(QStringLiteral("EA_DIAG")), QVariant(QStringLiteral("rejected")));
    QVERIFY(!result.bundle.isAvailable(attrKey("EA1")));
    QVERIFY(!result.bundle.isAvailable(attrKey("EA2")));
}

// Failed and MissingInput installs write nothing and delete nothing.
void ResultStoreTest::nonOkInstallWritesAndDeletesNothing()
{
    LogbookManager &logbook = LogbookManager::instance();
    QVERIFY(setInput("s1", "T_IN", 1));
    QVERIFY(waitForIdle(*m_model));

    StoredCalculationResult thrower;
    thrower.calculationId = kThrower;
    thrower.bundle.setAttribute(QStringLiteral("T_OUT"), 2);
    thrower.leaves = {GraphNode::storedAttribute(QStringLiteral("T_IN"))};
    thrower.inputFingerprint = QByteArray(InputFingerprintSize, 't');
    QVERIFY(logbook.writeCalculationRecord("s1", CalculationRecord::stamped(thrower)));

    StoredCalculationResult expA;
    expA.calculationId = kExpA;
    expA.bundle.setAttribute(QStringLiteral("EA1"), 5);
    expA.leaves = {GraphNode::storedAttribute(QStringLiteral("EA_IN"))};
    expA.inputFingerprint = QByteArray(InputFingerprintSize, 'a');
    QVERIFY(logbook.writeCalculationRecord("s2", CalculationRecord::stamped(expA)));

    const QString throwerPath = recordPath("s1", kThrower);
    const QString expAPath = recordPath("s2", QStringLiteral("exp%41"));
    const QByteArray throwerBytes = bytesOf(throwerPath);
    const QByteArray expABytes = bytesOf(expAPath);
    QVERIFY(!throwerBytes.isEmpty());
    QVERIFY(!expABytes.isEmpty());
    m_model->resetStoredResultStats();

    const JobQueue::RequestResult job = m_queue->request("s1", kThrower);
    QCOMPARE(job.kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(m_queue->job(job.job).state, JobState::Succeeded);
    QCOMPARE(m_queue->job(job.job).resultStatus, std::optional<ResultStatus>(ResultStatus::Failed));
    QCOMPARE(engine("s2").request(kExpA).status, ResultStatus::MissingInput);

    QCOMPARE(bytesOf(throwerPath), throwerBytes);
    QCOMPARE(bytesOf(expAPath), expABytes);
    QCOMPARE(stats().recordsWritten, 0);
    QCOMPARE(stats().writeFailures, 0);
    QCOMPARE(stats().droppedRecordsDeleted, 0);
}

void ResultStoreTest::recordBeforeFirstSave_data()
{
    QTest::addColumn<bool>("viaRestart");
    QTest::newRow("evict") << false;
    QTest::newRow("restart") << true;
}

// A session created by an import has a logbook identity at once (a reserved
// stem): a result published before the idle saver ran is stored under it,
// and the first save uses that stem.
void ResultStoreTest::recordBeforeFirstSave()
{
    QFETCH(bool, viaRestart);
    const QStringList recordsBefore = calculationRecordFiles();
    const QStringList csvBefore = sessionCsvFiles();

    QList<SessionData> incoming = JobWorld::sessions({"n1"});
    incoming[0].setAttribute(QStringLiteral("EA_IN"), 4);
    const QList<MergeResult> results = m_model->mergeSessions(incoming);
    QCOMPARE(results.size(), 1);
    QCOMPARE(results.at(0).outcome, MergeResult::Outcome::Created);
    m_model->resetStoredResultStats();
    {
        // Same event-loop pass: nothing has been saved
        WarningCapture warnings;
        QCOMPARE(engine("n1").request(kExpA).status, ResultStatus::Ok);
        QCOMPARE(warnings.messages(), QStringList());
    }
    QCOMPARE(stats().recordsWritten, 1);

    QStringList gained = calculationRecordFiles();
    for (const QString &name : recordsBefore)
        gained.removeOne(name);
    QCOMPARE(gained.size(), 1);
    QVERIFY2(gained.at(0).endsWith(QStringLiteral(".exp%41.fvresult")), qPrintable(gained.at(0)));
    QCOMPARE(sessionCsvFiles(), csvBefore);

    // A reservation is never listed in index.json
    QVERIFY(LogbookManager::instance().flushIndex());
    QVERIFY(!readIndex()[QStringLiteral("sessions")].toObject().contains(QStringLiteral("n1")));

    // The idle saver saves n1 under the reserved stem
    QVERIFY(waitForIdle(*m_model));
    const QString stem = sessionFileStem("n1");
    QVERIFY(!stem.isEmpty());
    QCOMPARE(gained.at(0), stem + QStringLiteral(".exp%41.fvresult"));
    QCOMPARE(sessionCsvFiles().size(), csvBefore.size() + 1);

    if (viaRestart) {
        restart();
    } else {
        QCOMPARE(evict({"n1"}), QString());
        m_model->resetStoredResultStats();
    }
    const Quiet quiet(*m_queue);
    const int jobs = m_queue->model()->rowCount();
    CalculationEngine &loaded = engine("n1");
    QCOMPARE(loaded.resultStatus(kExpA), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(session("n1").getAttribute(QStringLiteral("EA1")), QVariant(5));
    QCOMPARE(loaded.runCount(kExpA), 0);
    QCOMPARE(loaded.preparedCount(), 0);
    QCOMPARE(stats().recordsRestored, 1);
    QCOMPARE(m_queue->model()->rowCount(), jobs);
    QVERIFY(quiet.holds());
}

// A session that is never saved (a crash before the first save) leaves a
// stray record, which the next start removes.
void ResultStoreTest::recordOfNeverSavedSessionIsStray()
{
    const QStringList recordsBefore = calculationRecordFiles();
    const QStringList csvBefore = sessionCsvFiles();

    QList<SessionData> incoming = JobWorld::sessions({"n1"});
    incoming[0].setAttribute(QStringLiteral("EA_IN"), 4);
    QCOMPARE(m_model->mergeSessions(incoming).at(0).outcome, MergeResult::Outcome::Created);
    QCOMPARE(engine("n1").request(kExpA).status, ResultStatus::Ok);
    QStringList gained = calculationRecordFiles();
    for (const QString &name : recordsBefore)
        gained.removeOne(name);
    QCOMPARE(gained.size(), 1);

    // No event-loop pass: the model's destructor saves nothing
    m_queue->shutdown();
    m_queue.reset();
    m_model.reset();
    restart();

    QVERIFY(!calculationRecordFiles().contains(gained.at(0)));
    QCOMPARE(calculationRecordFiles(), recordsBefore);
    QVERIFY(row("n1") < 0);
    QCOMPARE(sessionCsvFiles(), csvBefore);
}

// The main window's delete sequence removes the records of a session that
// was never saved.
void ResultStoreTest::removeBeforeFirstSave()
{
    const QStringList recordsBefore = calculationRecordFiles();
    QList<SessionData> incoming = JobWorld::sessions({"n1"});
    incoming[0].setAttribute(QStringLiteral("EA_IN"), 4);
    QCOMPARE(m_model->mergeSessions(incoming).at(0).outcome, MergeResult::Outcome::Created);
    QCOMPARE(engine("n1").request(kExpA).status, ResultStatus::Ok);
    QCOMPARE(calculationRecordFiles().size(), recordsBefore.size() + 1);

    LogbookManager &logbook = LogbookManager::instance();
    QVERIFY(m_model->removeSessions({"n1"}));
    QVERIFY(logbook.removeSession("n1"));
    QVERIFY(logbook.flushIndex());

    QCOMPARE(calculationRecordFiles(), recordsBefore);
    QVERIFY(!logbook.removeSession("n1"));
}

// A directory at the record's path: the write fails once, loudly, and the
// in-memory result is untouched. The next Ok publish writes it.
void ResultStoreTest::writeFailureLeavesResultUsable()
{
    const QString path = recordPath("s1", QStringLiteral("exp%41"));
    QVERIFY(QDir().mkpath(path));
    const auto removeDirectory = qScopeGuard([path] { QDir().rmdir(path); });
    QVERIFY(setInput("s1", "EA_IN", 4));
    m_model->resetStoredResultStats();

    {
        WarningCapture warnings;
        QCOMPARE(engine("s1").request(kExpA).status, ResultStatus::Ok);
        QCOMPARE(warnings.count(), 1);
        QCOMPARE(warnings.count(QStringLiteral("not written")), 1);
    }
    QCOMPARE(engine("s1").resultStatus(kExpA), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(session("s1").getAttribute(QStringLiteral("EA1")), QVariant(5));
    QCOMPARE(session("s1").getAttribute(QStringLiteral("DA")), QVariant(105));
    QCOMPARE(stats().writeFailures, 1);
    QCOMPARE(stats().recordsWritten, 0);
    QVERIFY(QFileInfo(path).isDir());
    QCOMPARE(calculationRecordFiles(), QStringList());     // no temporary file left

    // Tried again at the next Ok publish
    QVERIFY(QDir().rmdir(path));
    QVERIFY(setInput("s1", "EA_IN", 6));
    QCOMPARE(engine("s1").request(kExpA).status, ResultStatus::Ok);
    QVERIFY(QFileInfo(path).isFile());
    QCOMPARE(stats().recordsWritten, 1);
}

// A result the record format refuses (a QVariantList attribute): the previous
// record keeps its bytes, the in-memory result is usable.
void ResultStoreTest::writeFailureKeepsPreviousRecord()
{
    QVERIFY(setInput("s1", "LY_IN", -1));
    QVERIFY(waitForIdle(*m_model));

    StoredCalculationResult previous;
    previous.calculationId = kListy;
    previous.bundle.setAttribute(QStringLiteral("LY_OUT"), 4);
    previous.leaves = {GraphNode::storedAttribute(QStringLiteral("LY_IN"))};
    previous.inputFingerprint = QByteArray(InputFingerprintSize, 'x');
    QVERIFY(LogbookManager::instance().writeCalculationRecord("s1", CalculationRecord::stamped(previous)));
    const QString path = recordPath("s1", QStringLiteral("test%2Estore%2Elisty"));
    const QByteArray bytes = bytesOf(path);
    QVERIFY(!bytes.isEmpty());
    m_model->resetStoredResultStats();

    {
        WarningCapture warnings;
        QCOMPARE(engine("s1").request(kListy).status, ResultStatus::Ok);
        QCOMPARE(warnings.count(), 1);
        QCOMPARE(warnings.count(QStringLiteral("LY_OUT")), 1);
    }
    const QVariant value = session("s1").getAttribute(QStringLiteral("LY_OUT"));
    QCOMPARE(value.typeId(), int(QMetaType::QVariantList));
    QCOMPARE(value.toList(), QVariantList{QVariant(-1)});
    QCOMPARE(bytesOf(path), bytes);
    QCOMPARE(stats().writeFailures, 1);
    QCOMPARE(stats().recordsWritten, 0);
}

// ---- Deleting ------------------------------------------------------------------------

// An input change deletes the record of every result it drops, at once; an
// edit the result does not depend on leaves the record alone.
void ResultStoreTest::inputChangeDeletesRecord()
{
    QVERIFY(setInput("s1", "EA_IN", 4));
    QVERIFY(setInput("s1", "EB_IN", 10));
    QCOMPARE(engine("s1").request(kExpA).status, ResultStatus::Ok);
    QCOMPARE(engine("s1").request(kExpB).status, ResultStatus::Ok);
    const QString pathA = recordPath("s1", QStringLiteral("exp%41"));
    const QString pathB = recordPath("s1", QStringLiteral("exp%42"));
    QVERIFY(QFileInfo(pathA).isFile());
    QVERIFY(QFileInfo(pathB).isFile());
    const int jobs = m_queue->model()->rowCount();
    m_model->resetStoredResultStats();

    // No event-loop pass in between
    QVERIFY(setInput("s1", "EA_IN", 7));
    QVERIFY(!QFileInfo::exists(pathA));
    QVERIFY(!QFileInfo::exists(pathB));
    QCOMPARE(stats().droppedRecordsDeleted, 2);
    QVERIFY(engine("s1").resultStatus(kExpA) != std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(m_queue->model()->rowCount(), jobs);

    // An attribute expA does not read
    QCOMPARE(engine("s1").request(kExpA).status, ResultStatus::Ok);
    const QByteArray bytes = bytesOf(pathA);
    QVERIFY(!bytes.isEmpty());
    QVERIFY(m_model->updateAttribute("s1", QStringLiteral("_DESCRIPTION"), QStringLiteral("x")));
    QCOMPARE(bytesOf(pathA), bytes);
    QCOMPARE(engine("s1").resultStatus(kExpA), std::optional<ResultStatus>(ResultStatus::Ok));
}

// Eviction, a registry change that does not reach the result and a restart
// never delete a record.
void ResultStoreTest::noDeleteWithoutInputChange()
{
    QVERIFY(setInput("s1", "EA_IN", 4));
    QCOMPARE(engine("s1").request(kExpA).status, ResultStatus::Ok);
    QVERIFY(waitForIdle(*m_model));
    const QString path = recordPath("s1", QStringLiteral("exp%41"));
    const QByteArray bytes = bytesOf(path);
    QVERIFY(!bytes.isEmpty());

    // (a) eviction
    QCOMPARE(evict({"s1"}), QString());
    QCOMPARE(bytesOf(path), bytes);

    // (b) a registry change that does not reach the result (an output expA
    // never looks up) drops neither it nor the record
    QCOMPARE(engine("s1").resultStatus(kExpA), std::optional<ResultStatus>(ResultStatus::Ok));
    m_model->resetStoredResultStats();
    bool shadowRegistered = false;
    const auto unregisterShadow = qScopeGuard([&shadowRegistered] {
        if (shadowRegistered)
            CalculationRegistry::instance().unregister(kShadow, CalculationRegistry::Removal::Change);
    });
    shadowRegistered = CalculationRegistry::instance().registerCalculation(
        constantCalculation(kShadow, QStringLiteral("_STORE_SHADOW"), 0));
    QVERIFY(shadowRegistered);
    QCOMPARE(engine("s1").resultStatus(kExpA), std::optional<ResultStatus>(ResultStatus::Ok));
    QVERIFY(CalculationRegistry::instance().unregister(kShadow, CalculationRegistry::Removal::Change));
    shadowRegistered = false;
    m_model->flushPendingInvalidations();
    QCOMPARE(bytesOf(path), bytes);
    QCOMPARE(stats().droppedRecordsDeleted, 0);

    // (c) a restart
    restart();
    QCOMPARE(bytesOf(path), bytes);
    QCOMPARE(engine("s1").resultStatus(kExpA), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(stats().recordsRestored, 1);
    QCOMPARE(bytesOf(path), bytes);
}

// An explicit family instance ("test.store.fam#EA_IN") is not stored: the
// store ignores its events, so its Ok install writes nothing and warns
// nothing, and its drop by an input change removes nothing.
void ResultStoreTest::explicitFamilyIsNotStored()
{
    CalculationFamily family;
    family.id = kFamily;
    family.policy = EvaluationPolicy::Explicit;
    family.instantiate = [](const DependencyKey &name) -> std::optional<CalculationDescriptor> {
        const QString prefix = QStringLiteral("FAM:");
        if (name.type != DependencyKey::Type::Attribute || !name.attributeKey.startsWith(prefix))
            return std::nullopt;
        const QString output = name.attributeKey;
        const QString source = output.mid(prefix.size());
        if (source.isEmpty())
            return std::nullopt;
        CalculationDescriptor d;
        d.id = source;      // instance key
        d.inputs = {CalcInput::attribute(source)};
        d.outputs = {DependencyKey::attribute(output)};
        d.compute = [output, source](const EvaluationContext &ctx) {
            return CalculationResult().setAttribute(output, -ctx.attribute(source).toInt());
        };
        return d;
    };
    bool registered = false;
    const auto unregisterFamily = qScopeGuard([&registered] {
        if (registered)
            CalculationRegistry::instance().unregister(kFamily, CalculationRegistry::Removal::Change);
    });
    registered = CalculationRegistry::instance().registerFamily(family);
    QVERIFY(registered);
    m_model->flushPendingInvalidations();

    const DependencyKey name = attrKey("FAM:EA_IN");
    QVERIFY(setInput("s1", "EA_IN", 4));
    QVERIFY(waitForIdle(*m_model));
    const QStringList recordsBefore = calculationRecordFiles();
    m_model->resetStoredResultStats();

    {
        WarningCapture warnings;
        QCOMPARE(engine("s1").request(kFamily, name).status, ResultStatus::Ok);
        QCOMPARE(warnings.messages(), QStringList());
    }
    QCOMPARE(session("s1").getAttribute(QStringLiteral("FAM:EA_IN")), QVariant(-4));
    QVERIFY(!engine("s1").exportResult(kFamily + QStringLiteral("#EA_IN")).has_value());
    QCOMPARE(calculationRecordFiles(), recordsBefore);
    QCOMPARE(stats().recordsWritten, 0);
    QCOMPARE(stats().writeFailures, 0);

    // Its drop by an input change
    QVERIFY(setInput("s1", "EA_IN", 5));
    QVERIFY(engine("s1").resultStatus(kFamily, name) != std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(stats().droppedRecordsDeleted, 0);
    QCOMPARE(calculationRecordFiles(), recordsBefore);

    // Nothing to restore: after an eviction it reads not requested again
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(evict({"s1"}), QString());
    QVERIFY(engine("s1").resultStatus(kFamily, name) != std::optional<ResultStatus>(ResultStatus::Ok));
    QVERIFY(!session("s1").getAttribute(QStringLiteral("FAM:EA_IN")).isValid());
    QCOMPARE(stats().recordsRestored, 0);
}

// ---- Restoring -----------------------------------------------------------------------

void ResultStoreTest::restoreOnEveryLoadPath_data()
{
    QTest::addColumn<QString>("path");
    for (const char *path : {"show", "background", "focus", "edit", "startup", "mergeUnloaded"})
        QTest::newRow(path) << QString::fromLatin1(path);
}

// Every load path that installs s1 into its row restores the stored result
// before sessionLoaded, without a run or a job, and never rewrites the record.
void ResultStoreTest::restoreOnEveryLoadPath()
{
    QFETCH(QString, path);
    QVERIFY(setInput("s1", "EA_IN", 4));
    QCOMPARE(engine("s1").request(kExpA).status, ResultStatus::Ok);
    QVERIFY(waitForIdle(*m_model));
    const QString recordFile = recordPath("s1", QStringLiteral("exp%41"));
    const QByteArray bytes = bytesOf(recordFile);
    QVERIFY(!bytes.isEmpty());
    QCOMPARE(evict({"s1", "s2", "s3", "s4"}), QString());

    if (path == QLatin1String("startup"))
        restart();
    m_model->resetStoredResultStats();
    QObject scope;
    LoadWatch atLoad;
    watchLoad(&scope, "s1", kExpA, &atLoad);
    const Quiet quiet(*m_queue);

    if (path == QLatin1String("show") || path == QLatin1String("startup")) {
        m_model->setRowsVisibility({{row("s1"), true}});
    } else if (path == QLatin1String("background")) {
        QMap<int, bool> all;
        for (const char *id : {"s1", "s2", "s3", "s4"})
            all.insert(row(QString::fromLatin1(id)), true);
        m_model->setRowsVisibility(all);
        QVERIFY(waitForIdle(*m_model));
    } else if (path == QLatin1String("focus")) {
        m_model->setFocusedSessionId("s1");
    } else if (path == QLatin1String("edit")) {
        QVERIFY(m_model->updateAttribute("s1", QStringLiteral("_DESCRIPTION"), QStringLiteral("renamed")));
    } else if (path == QLatin1String("mergeUnloaded")) {
        SessionData incoming;
        incoming.setAttribute(QStringLiteral("SESSION_ID"), QStringLiteral("s1"));
        incoming.setAttribute(QStringLiteral("STORE_NOTE"), QStringLiteral("n"));
        const QList<MergeResult> results = m_model->mergeSessions(QList<SessionData>{incoming});
        QCOMPARE(results.size(), 1);
        QCOMPARE(results.at(0).outcome, MergeResult::Outcome::Merged);
    } else {
        QFAIL("unknown path");
    }

    QVERIFY(isLoaded("s1"));
    QVERIFY(atLoad.seen);
    QCOMPARE(atLoad.status, std::optional<ResultStatus>(ResultStatus::Ok));
    CalculationEngine &loaded = engine("s1");
    QCOMPARE(loaded.resultStatus(kExpA), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(session("s1").getAttribute(QStringLiteral("EA1")), QVariant(5));
    QCOMPARE(loaded.runCount(kExpA), 0);
    QCOMPARE(loaded.preparedCount(), 0);
    QCOMPARE(stats().recordsRestored, 1);
    QCOMPARE(stats().recordsWritten, 0);
    QCOMPARE(bytesOf(recordFile), bytes);
    QVERIFY(quiet.holds());

    // Only the session with a record was listed; s2..s4 (no record) were not
    QCOMPARE(stats().recordListings, 1);
    if (path == QLatin1String("background"))
        QCOMPARE(stats().restoreCalls, 4);
}

// The bulk edit's temporary session becomes the row when its save fails: that
// is an install, so it restores.
void ResultStoreTest::bulkEditPromotionRestores()
{
    TestEnvironment &env = TestEnvironment::instance();
    QVERIFY(setInput("s1", "EA_IN", 4));
    QCOMPARE(engine("s1").request(kExpA).status, ResultStatus::Ok);
    QVERIFY(waitForIdle(*m_model));     // the index holds the description column's value
    const QString recordFile = recordPath("s1", QStringLiteral("exp%41"));
    const QByteArray bytes = bytesOf(recordFile);
    QVERIFY(!bytes.isEmpty());
    QCOMPARE(evict({"s1"}), QString());
    m_model->resetStoredResultStats();

    // A directory at index.json: saveSession()'s pre-save flush fails
    QVERIFY(QFile::remove(env.indexPath()));
    QVERIFY(QDir().mkdir(env.indexPath()));
    const auto removeDirectory = qScopeGuard([&env] { QDir().rmdir(env.indexPath()); });
    {
        WarningCapture warnings;
        m_model->startBulkEdit({row("s1")}, 0, QStringLiteral("bulk"));
        QVERIFY(waitForIdle(*m_model));
        QVERIFY(warnings.count() > 0);
    }

    const SessionRow &sr = std::as_const(*m_model).rowAt(row("s1"));
    QVERIFY(sr.isLoaded());
    QVERIFY(sr.dirty);
    QVERIFY(sr.saveFailed);
    CalculationEngine &loaded = engine("s1");
    QCOMPARE(loaded.resultStatus(kExpA), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(session("s1").getAttribute(QStringLiteral("EA1")), QVariant(5));
    QCOMPARE(loaded.runCount(kExpA), 0);
    QCOMPARE(loaded.preparedCount(), 0);
    QCOMPARE(stats().recordsRestored, 1);
    QCOMPARE(bytesOf(recordFile), bytes);
}

// An explicit result that reads another one restores in any file order.
void ResultStoreTest::restoresChainInPasses()
{
    QVERIFY(setInput("s1", "UP_IN", 2));
    QVERIFY(setInput("s1", "DOWN_IN", 5));
    QCOMPARE(engine("s1").request(kUp).status, ResultStatus::Ok);
    QCOMPARE(engine("s1").request(kDown).status, ResultStatus::Ok);
    QCOMPARE(evict({"s1"}), QString());

    // The premise: the downstream record comes first
    QCOMPARE(LogbookManager::instance().calculationRecordIds("s1"), QStringList({kDown, kUp}));
    m_model->resetStoredResultStats();
    const Quiet quiet(*m_queue);

    CalculationEngine &loaded = engine("s1");
    QCOMPARE(loaded.resultStatus(kUp), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(loaded.resultStatus(kDown), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(session("s1").getAttribute(QStringLiteral("DOWN_OUT")), QVariant(11));
    QCOMPARE(loaded.runCount(kUp), 0);
    QCOMPARE(loaded.runCount(kDown), 0);
    QCOMPARE(loaded.preparedCount(), 0);
    QCOMPARE(stats().recordsRestored, 2);
    QCOMPARE(stats().staleRecordsDeleted, 0);
    QVERIFY(quiet.holds());
}

// Inputs that stay unavailable after the last pass make a record stale.
void ResultStoreTest::upstreamMissingAfterLastPassDeletes()
{
    QVERIFY(setInput("s1", "UP_IN", 2));
    QVERIFY(setInput("s1", "DOWN_IN", 5));
    QCOMPARE(engine("s1").request(kUp).status, ResultStatus::Ok);
    QCOMPARE(engine("s1").request(kDown).status, ResultStatus::Ok);
    QCOMPARE(evict({"s1"}), QString());
    const QString downPath = recordPath("s1", QStringLiteral("test%2Estore%2Edown"));
    QVERIFY(QFileInfo(downPath).isFile());
    QVERIFY(LogbookManager::instance().removeCalculationRecord("s1", kUp));
    m_model->resetStoredResultStats();
    const Quiet quiet(*m_queue);

    CalculationEngine &loaded = engine("s1");
    QVERIFY(!QFileInfo::exists(downPath));
    QVERIFY(loaded.resultStatus(kDown) != std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(stats().staleRecordsDeleted, 1);
    QCOMPARE(stats().recordsRestored, 0);
    QCOMPARE(loaded.runCount(kDown), 0);
    QCOMPARE(loaded.runCount(kUp), 0);
    QCOMPARE(loaded.preparedCount(), 0);
    QVERIFY(quiet.holds());
}

void ResultStoreTest::staleRecordDeletedOnLoad_data()
{
    QTest::addColumn<QString>("alteration");
    for (const char *alteration : {"compatibility", "resolutions", "resultVersion", "bundle", "leaves",
                                   "fingerprint", "notARecord", "unsupportedVersion", "unknownCalculation"})
        QTest::newRow(alteration) << QString::fromLatin1(alteration);
}

// Every reason a record is stale deletes it when the session is loaded; the
// calculation then reads not requested, and nothing runs.
void ResultStoreTest::staleRecordDeletedOnLoad()
{
    QFETCH(QString, alteration);
    QVERIFY(setInput("s1", "EA_IN", 4));
    QCOMPARE(engine("s1").request(kExpA).status, ResultStatus::Ok);
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(evict({"s1"}), QString());
    const QString expAPath = recordPath("s1", QStringLiteral("exp%41"));
    QString alteredPath = expAPath;

    if (alteration == QLatin1String("compatibility")) {
        QCOMPARE(rewriteRecord("s1", kExpA, [](CalculationRecord &r) { r.calculationCompatibility += 1; }),
                 QString());
    } else if (alteration == QLatin1String("resolutions")) {
        // The premise: expA looked up EA_IN only, and the session provided it
        const std::optional<CalculationRecord> record = storedRecord("s1", kExpA);
        QVERIFY(record.has_value());
        QCOMPARE(record->result.resolutions.size(), 1);
        QVERIFY(hasResolution(record->result.resolutions, attrKey("EA_IN"), StoredResolution::Provider::SessionData));
        QCOMPARE(rewriteRecord("s1", kExpA, [](CalculationRecord &r) {
                     r.result.resolutions.first().provider = StoredResolution::Provider::Nothing;
                 }), QString());
    } else if (alteration == QLatin1String("resultVersion")) {
        QCOMPARE(rewriteRecord("s1", kExpA, [](CalculationRecord &r) { r.result.resultVersion = QStringLiteral("v-old"); }),
                 QString());
    } else if (alteration == QLatin1String("bundle")) {
        QCOMPARE(rewriteRecord("s1", kExpA, [](CalculationRecord &r) {
                     r.result.bundle.setAttribute(QStringLiteral("NOT_DECLARED"), 1);
                 }), QString());
    } else if (alteration == QLatin1String("leaves")) {
        const CalculationRecordRead read = LogbookManager::instance().readCalculationRecord("s1", kExpA);
        QCOMPARE(read.status, CalculationRecordStatus::Ok);
        QCOMPARE(read.record->result.leaves.size(), 1);
        QCOMPARE(rewriteRecord("s1", kExpA, [](CalculationRecord &r) { r.result.leaves.clear(); }), QString());
    } else if (alteration == QLatin1String("fingerprint")) {
        QCOMPARE(rewriteRecord("s1", kExpA, [](CalculationRecord &r) {
                     r.result.inputFingerprint[0] = char(~r.result.inputFingerprint.at(0));
                 }), QString());
    } else if (alteration == QLatin1String("notARecord")) {
        QVERIFY(writeBytes(expAPath, QByteArrayLiteral("garbage")));
    } else if (alteration == QLatin1String("unsupportedVersion")) {
        QByteArray bytes = bytesOf(expAPath);
        QVERIFY(bytes.size() > 12);
        bytes.replace(8, 4, QByteArray("\x03\x00\x00\x00", 4));
        QVERIFY(writeBytes(expAPath, bytes));
    } else if (alteration == QLatin1String("unknownCalculation")) {
        QCOMPARE(rewriteRecord("s1", kExpA, [](CalculationRecord &r) { r.result.calculationId = kGone; }),
                 QString());
        alteredPath = recordPath("s1", QStringLiteral("test%2Estore%2Egone"));
        QVERIFY(QFileInfo(expAPath).isFile());
    } else {
        QFAIL("unknown alteration");
    }
    QVERIFY(QFileInfo(alteredPath).isFile());
    m_model->resetStoredResultStats();
    const Quiet quiet(*m_queue);

    CalculationEngine &loaded = engine("s1");
    QVERIFY(!QFileInfo::exists(alteredPath));
    QCOMPARE(stats().staleRecordsDeleted, 1);
    QCOMPARE(loaded.runCount(kExpA), 0);
    QCOMPARE(loaded.preparedCount(), 0);
    if (alteration == QLatin1String("unknownCalculation")) {
        QCOMPARE(loaded.resultStatus(kExpA), std::optional<ResultStatus>(ResultStatus::Ok));
        QCOMPARE(stats().recordsRestored, 1);
        QVERIFY(QFileInfo(expAPath).isFile());
    } else {
        QVERIFY(loaded.resultStatus(kExpA) != std::optional<ResultStatus>(ResultStatus::Ok));
        QCOMPARE(stats().recordsRestored, 0);
    }
    QVERIFY(quiet.holds());
}

// A record in format version 1 (the environment fingerprint as a second
// stamp) is deleted as stale at load, and nothing is skipped or run.
void ResultStoreTest::formatOneRecordIsDeletedOnLoad()
{
    QVERIFY(setInput("s1", "EA_IN", 4));
    QCOMPARE(engine("s1").request(kExpA).status, ResultStatus::Ok);
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(evict({"s1"}), QString());
    const QString path = recordPath("s1", QStringLiteral("exp%41"));
    const QByteArray formatOne = asFormatOne(bytesOf(path));
    QVERIFY(!formatOne.isEmpty());
    QVERIFY(writeBytes(path, formatOne));
    const CalculationRecordRead read = LogbookManager::instance().readCalculationRecord("s1", kExpA);
    QCOMPARE(read.status, CalculationRecordStatus::UnsupportedVersion);
    QCOMPARE(read.error, QStringLiteral("format version 1 is not supported"));
    m_model->resetStoredResultStats();
    const Quiet quiet(*m_queue);
    WarningCapture warnings;

    CalculationEngine &loaded = engine("s1");
    QVERIFY(!QFileInfo::exists(path));
    QCOMPARE(stats().staleRecordsDeleted, 1);
    QCOMPARE(stats().recordsRestored, 0);
    QCOMPARE(stats().recordsSkipped, 0);
    QVERIFY(loaded.resultStatus(kExpA) != std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(loaded.runCount(kExpA), 0);
    QCOMPARE(warnings.count(QStringLiteral("skipped")), 0);
    QVERIFY(quiet.holds());
}

void ResultStoreTest::unreadableRecordIsSkipped_data()
{
    QTest::addColumn<int>("mechanism");
    QTest::addColumn<bool>("viaRestart");

    QTest::newRow("directory") << int(UnreadableFile::Mechanism::Directory) << false;
    QTest::newRow("locked without sharing") << int(UnreadableFile::Mechanism::LockedWithoutSharing) << false;
    QTest::newRow("locked without sharing, restart") << int(UnreadableFile::Mechanism::LockedWithoutSharing) << true;
    QTest::newRow("no read permission") << int(UnreadableFile::Mechanism::NoReadPermission) << false;
    QTest::newRow("no read permission, restart") << int(UnreadableFile::Mechanism::NoReadPermission) << true;
}

// A record that exists but cannot be read is kept, not restored: the
// calculation reads not requested, the pair is unconfirmed, and one warning
// says so. Once readable, the next load restores it.
void ResultStoreTest::unreadableRecordIsSkipped()
{
    QFETCH(int, mechanism);
    QFETCH(bool, viaRestart);
    LogbookManager &logbook = LogbookManager::instance();

    QVERIFY(setInput("s1", "EA_IN", 4));
    QCOMPARE(engine("s1").request(kExpA).status, ResultStatus::Ok);
    QVERIFY(waitForIdle(*m_model));
    const QString path = recordPath("s1", QStringLiteral("exp%41"));
    const QByteArray r0 = bytesOf(path);
    QVERIFY(!r0.isEmpty());
    QCOMPARE(evict({"s1"}), QString());

    UnreadableFile unreadable(path, UnreadableFile::Mechanism(mechanism));
    if (!unreadable.skipReason().isEmpty())
        QSKIP(qPrintable(unreadable.skipReason()));
    if (viaRestart) {
        restart();
        QVERIFY(logbook.knownCalculationRecords("s1").contains(kExpA));
    }

    m_model->resetStoredResultStats();
    {
        const Quiet quiet(*m_queue);
        WarningCapture warnings;
        session("s1");

        CalculationEngine &loaded = engine("s1");
        QVERIFY(loaded.resultStatus(kExpA) != std::optional<ResultStatus>(ResultStatus::Ok));
        QVERIFY(!session("s1").getAttribute(QStringLiteral("EA1")).isValid());
        QCOMPARE(loaded.runCount(kExpA), 0);
        QCOMPARE(loaded.preparedCount(), 0);
        QCOMPARE(stats().recordsSkipped, 1);
        QCOMPARE(stats().staleRecordsDeleted, 0);
        QCOMPARE(stats().recordsRestored, 0);
        QCOMPARE(stats().recordsWritten, 0);
        QCOMPARE(warnings.count(kSkipped), 1);
        QVERIFY(QFileInfo::exists(path));
        QCOMPARE(logbook.unconfirmedCalculationRecords("s1"), QSet<QString>({kExpA}));
        QVERIFY(logbook.knownCalculationRecords("s1").contains(kExpA));
        QVERIFY(quiet.holds());
    }

    // Readable again: eviction forgets the mark, and the next load restores
    QVERIFY(unreadable.release());
    QCOMPARE(bytesOf(path), r0);
    QCOMPARE(evict({"s1"}), QString());
    QVERIFY(logbook.unconfirmedCalculationRecords("s1").isEmpty());
    m_model->resetStoredResultStats();
    CalculationEngine &loaded = engine("s1");
    QCOMPARE(loaded.resultStatus(kExpA), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(session("s1").getAttribute(QStringLiteral("EA1")), QVariant(5));
    QCOMPARE(loaded.runCount(kExpA), 0);
    QCOMPARE(stats().recordsRestored, 1);
    QCOMPARE(stats().recordsSkipped, 0);
    QCOMPARE(bytesOf(path), r0);
}

// A readable record whose only obstacle is a skipped upstream record is
// skipped too, not deleted; both come back once the upstream one is readable.
void ResultStoreTest::dependentOfSkippedRecordIsKept()
{
    LogbookManager &logbook = LogbookManager::instance();
    QVERIFY(setInput("s1", "UP_IN", 2));
    QVERIFY(setInput("s1", "DOWN_IN", 5));
    QCOMPARE(engine("s1").request(kUp).status, ResultStatus::Ok);
    QCOMPARE(engine("s1").request(kDown).status, ResultStatus::Ok);
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(evict({"s1"}), QString());
    const QString upPath = recordPath("s1", QStringLiteral("test%2Estore%2Eup"));
    const QString downPath = recordPath("s1", QStringLiteral("test%2Estore%2Edown"));
    const QByteArray upBytes = bytesOf(upPath);
    const QByteArray downBytes = bytesOf(downPath);
    QVERIFY(!upBytes.isEmpty());
    QVERIFY(!downBytes.isEmpty());

    UnreadableFile unreadable(upPath, UnreadableFile::Mechanism::Directory);
    if (!unreadable.skipReason().isEmpty())
        QSKIP(qPrintable(unreadable.skipReason()));

    m_model->resetStoredResultStats();
    {
        WarningCapture warnings;
        CalculationEngine &loaded = engine("s1");
        QVERIFY(loaded.resultStatus(kUp) != std::optional<ResultStatus>(ResultStatus::Ok));
        QVERIFY(loaded.resultStatus(kDown) != std::optional<ResultStatus>(ResultStatus::Ok));
        QCOMPARE(stats().recordsSkipped, 2);
        QCOMPARE(stats().staleRecordsDeleted, 0);
        QCOMPARE(stats().recordsRestored, 0);
        QCOMPARE(bytesOf(downPath), downBytes);
        const QStringList skipped = warnings.matching(kSkipped);
        QCOMPARE(skipped.size(), 2);
        QCOMPARE(warnings.count(QStringLiteral("it reads a stored result that could not be read")), 1);
        QVERIFY(skipped.filter(QStringLiteral("it reads a stored result that could not be read"))
                    .at(0).contains(kDown));
        QCOMPARE(logbook.unconfirmedCalculationRecords("s1"), QSet<QString>({kUp, kDown}));
    }

    QVERIFY(unreadable.release());
    QCOMPARE(bytesOf(upPath), upBytes);
    QCOMPARE(evict({"s1"}), QString());
    m_model->resetStoredResultStats();
    CalculationEngine &loaded = engine("s1");
    QCOMPARE(loaded.resultStatus(kUp), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(loaded.resultStatus(kDown), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(session("s1").getAttribute(QStringLiteral("DOWN_OUT")), QVariant(11));
    QCOMPARE(loaded.runCount(kUp), 0);
    QCOMPARE(loaded.runCount(kDown), 0);
    QCOMPARE(stats().recordsRestored, 2);
}

void ResultStoreTest::restoresUpstreamFirst_data()
{
    QTest::addColumn<QString>("upstream");
    QTest::addColumn<QString>("downstream");

    QTest::newRow("downstream id first") << QStringLiteral("test.store.z.upstream")
                                         << QStringLiteral("test.store.a.downstream");
    QTest::newRow("upstream id first") << QStringLiteral("test.store.a.upstream")
                                       << QStringLiteral("test.store.z.downstream");
}

// A requested calculation F (downstream) reads Y, whose first candidate reads
// the output of another requested calculation E (upstream) and whose second
// does not. F's record names E through its resolutions, so it is restored
// after E whatever the id order: restored first, Y would resolve to the second
// candidate and the record would be refused. While E's record is skipped,
// F's is skipped too, not deleted, and both come back once E's is readable.
void ResultStoreTest::restoresUpstreamFirst()
{
    QFETCH(QString, upstream);
    QFETCH(QString, downstream);
    LogbookManager &logbook = LogbookManager::instance();
    const QString y1 = QStringLiteral("test.store.y1");
    const QString y2 = QStringLiteral("test.store.y2");
    QVERIFY(m_extra->add(timesExplicit(upstream, QStringLiteral("E_IN"), QStringLiteral("E_OUT"), 3)));
    QVERIFY(m_extra->add(plusOne(y1, QStringLiteral("E_OUT"), QStringLiteral("Y"))));        // tried first
    QVERIFY(m_extra->add(plusOne(y2, QStringLiteral("Y_ALT_IN"), QStringLiteral("Y"))));
    QVERIFY(m_extra->add(timesExplicit(downstream, QStringLiteral("Y"), QStringLiteral("F_OUT"), 10)));
    m_model->flushPendingInvalidations();

    QVERIFY(setInput("s1", "E_IN", 2));
    QVERIFY(setInput("s1", "Y_ALT_IN", 100));
    QCOMPARE(engine("s1").request(upstream).status, ResultStatus::Ok);
    QCOMPARE(engine("s1").request(downstream).status, ResultStatus::Ok);
    QCOMPARE(session("s1").getAttribute(QStringLiteral("F_OUT")), QVariant(70));     // (2 * 3 + 1) * 10
    const std::optional<CalculationRecord> record = storedRecord("s1", downstream);
    QVERIFY(record.has_value());
    QVERIFY(hasResolution(record->result.resolutions, attrKey("Y"), StoredResolution::Provider::Calculation, y1));
    QVERIFY(hasResolution(record->result.resolutions, attrKey("E_OUT"), StoredResolution::Provider::Calculation,
                          upstream));
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(evict({"s1"}), QString());
    const QString upPath = recordPath("s1", encodeRecordFileId(upstream));
    const QString downPath = recordPath("s1", encodeRecordFileId(downstream));
    const QByteArray upBytes = bytesOf(upPath);
    const QByteArray downBytes = bytesOf(downPath);
    QVERIFY(!upBytes.isEmpty());
    QVERIFY(!downBytes.isEmpty());
    // The premise: the ids sort as the row says
    QStringList inIdOrder{upstream, downstream};
    inIdOrder.sort();
    QCOMPARE(logbook.calculationRecordIds("s1"), inIdOrder);

    // 1. Both restored, nothing run
    m_model->resetStoredResultStats();
    {
        const Quiet quiet(*m_queue);
        CalculationEngine &loaded = engine("s1");
        QCOMPARE(loaded.resultStatus(upstream), std::optional<ResultStatus>(ResultStatus::Ok));
        QCOMPARE(loaded.resultStatus(downstream), std::optional<ResultStatus>(ResultStatus::Ok));
        QCOMPARE(session("s1").getAttribute(QStringLiteral("F_OUT")), QVariant(70));
        QCOMPARE(loaded.runCount(upstream), 0);
        QCOMPARE(loaded.runCount(downstream), 0);
        QCOMPARE(loaded.preparedCount(), 0);
        QCOMPARE(stats().recordsRestored, 2);
        QCOMPARE(stats().staleRecordsDeleted, 0);
        QVERIFY(quiet.holds());
    }
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(evict({"s1"}), QString());
    QCOMPARE(bytesOf(downPath), downBytes);

    // 2. The upstream record cannot be read: the downstream one is kept
    UnreadableFile unreadable(upPath, UnreadableFile::Mechanism::Directory);
    if (!unreadable.skipReason().isEmpty())
        QSKIP(qPrintable(unreadable.skipReason()));
    m_model->resetStoredResultStats();
    {
        WarningCapture warnings;
        CalculationEngine &loaded = engine("s1");
        QVERIFY(loaded.resultStatus(upstream) != std::optional<ResultStatus>(ResultStatus::Ok));
        QVERIFY(loaded.resultStatus(downstream) != std::optional<ResultStatus>(ResultStatus::Ok));
        QCOMPARE(loaded.runCount(downstream), 0);
        QCOMPARE(stats().recordsSkipped, 2);
        QCOMPARE(stats().staleRecordsDeleted, 0);
        QCOMPARE(stats().recordsRestored, 0);
        QCOMPARE(bytesOf(downPath), downBytes);
        const QStringList reads = warnings.matching(QStringLiteral("it reads a stored result that could not be read"));
        QCOMPARE(reads.size(), 1);
        QVERIFY(reads.at(0).contains(downstream));
        QCOMPARE(logbook.unconfirmedCalculationRecords("s1"), QSet<QString>({upstream, downstream}));
    }

    // 3. Readable again: both restored
    QVERIFY(unreadable.release());
    QCOMPARE(bytesOf(upPath), upBytes);
    QCOMPARE(evict({"s1"}), QString());
    m_model->resetStoredResultStats();
    CalculationEngine &loaded = engine("s1");
    QCOMPARE(loaded.resultStatus(upstream), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(loaded.resultStatus(downstream), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(session("s1").getAttribute(QStringLiteral("F_OUT")), QVariant(70));
    QCOMPARE(loaded.runCount(downstream), 0);
    QCOMPARE(stats().recordsRestored, 2);
}

void ResultStoreTest::skippedRecordIsReplacedByPublish_data()
{
    QTest::addColumn<int>("mechanism");

    QTest::newRow("directory") << int(UnreadableFile::Mechanism::Directory);
    QTest::newRow("locked without sharing") << int(UnreadableFile::Mechanism::LockedWithoutSharing);
    QTest::newRow("no read permission") << int(UnreadableFile::Mechanism::NoReadPermission);
}

// A record skipped at a load is replaced by the next publish of the pair once
// the file can be written again: the new bytes are on disk, the pair is no
// longer unconfirmed, and index.json gets the value of a column over it.
void ResultStoreTest::skippedRecordIsReplacedByPublish()
{
    QFETCH(int, mechanism);
    LogbookManager &logbook = LogbookManager::instance();
    LogbookColumn ea1Column;
    ea1Column.type = ColumnType::SessionAttribute;
    ea1Column.attributeKey = QStringLiteral("EA1");
    LogbookColumnStore::instance().setColumns({descriptionColumn(), ea1Column});    // cleanup() restores

    QVERIFY(setInput("s1", "EA_IN", 4));
    QCOMPARE(engine("s1").request(kExpA).status, ResultStatus::Ok);
    QVERIFY(waitForIdle(*m_model));
    const QString path = recordPath("s1", QStringLiteral("exp%41"));
    const QByteArray r0 = bytesOf(path);
    QVERIFY(!r0.isEmpty());
    QCOMPARE(evict({"s1"}), QString());

    UnreadableFile unreadable(path, UnreadableFile::Mechanism(mechanism));
    if (!unreadable.skipReason().isEmpty())
        QSKIP(qPrintable(unreadable.skipReason()));
    m_model->resetStoredResultStats();
    {
        WarningCapture warnings;
        session("s1");
        QCOMPARE(stats().recordsSkipped, 1);
        QCOMPARE(logbook.unconfirmedCalculationRecords("s1"), QSet<QString>({kExpA}));
    }
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(logbook.flushIndex());
    QVERIFY(indexValue("s1", ea1Column).isUndefined());

    // Writable again; a new publish with another input
    QVERIFY(unreadable.release());
    QVERIFY(setInput("s1", "EA_IN", 7));
    QCOMPARE(engine("s1").request(kExpA).status, ResultStatus::Ok);
    QCOMPARE(session("s1").getAttribute(QStringLiteral("EA1")), QVariant(8));
    QVERIFY(waitForIdle(*m_model));

    const QByteArray r1 = bytesOf(path);
    QVERIFY(!r1.isEmpty());
    QVERIFY(r1 != r0);
    QCOMPARE(stats().recordsWritten, 1);
    const std::optional<CalculationRecord> record = storedRecord("s1", kExpA);
    QVERIFY(record.has_value());
    QCOMPARE(record->result.bundle.attributeValue(QStringLiteral("EA1")), QVariant(8));
    QVERIFY(logbook.unconfirmedCalculationRecords("s1").isEmpty());
    QVERIFY(logbook.knownCalculationRecords("s1").contains(kExpA));
    QVERIFY(logbook.flushIndex());
    const QJsonValue value = indexValue("s1", ea1Column);
    QVERIFY(value.isDouble());
    QCOMPARE(value.toDouble(), 8.0);
}

void ResultStoreTest::deletingSessionWithSkippedRecord_data()
{
    QTest::addColumn<int>("mechanism");

    QTest::newRow("directory") << int(UnreadableFile::Mechanism::Directory);
    QTest::newRow("locked without sharing") << int(UnreadableFile::Mechanism::LockedWithoutSharing);
    QTest::newRow("no read permission") << int(UnreadableFile::Mechanism::NoReadPermission);
}

// Deleting a session while its record is skipped (DATA_SCHEMA section 12):
// a file that can be removed goes with the session; a file locked without
// sharing stays behind as a stray, which the next start removes once the lock
// is gone; a directory at the record's path is no record, is never listed,
// and stays.
void ResultStoreTest::deletingSessionWithSkippedRecord()
{
    QFETCH(int, mechanism);
    const auto kind = UnreadableFile::Mechanism(mechanism);
    LogbookManager &logbook = LogbookManager::instance();

    QVERIFY(setInput("s1", "EA_IN", 4));
    QCOMPARE(engine("s1").request(kExpA).status, ResultStatus::Ok);
    QVERIFY(waitForIdle(*m_model));
    const QString stem = sessionFileStem("s1");
    const QString csv = sessionFilePath("s1");
    const QString path = recordPath("s1", QStringLiteral("exp%41"));
    QVERIFY(QFileInfo(path).isFile());
    QCOMPARE(evict({"s1"}), QString());

    UnreadableFile unreadable(path, kind);
    if (!unreadable.skipReason().isEmpty())
        QSKIP(qPrintable(unreadable.skipReason()));
    m_model->resetStoredResultStats();
    {
        WarningCapture warnings;
        session("s1");
        QCOMPARE(stats().recordsSkipped, 1);
    }

    {
        WarningCapture warnings;
        QVERIFY(m_model->removeSessions({"s1"}));
        QVERIFY(logbook.removeSession("s1"));
        QVERIFY(logbook.flushIndex());
        QCOMPARE(warnings.count(QStringLiteral("not all removed")),
                 kind == UnreadableFile::Mechanism::LockedWithoutSharing ? 1 : 0);
    }
    QVERIFY(!QFileInfo::exists(csv));

    switch (kind) {
    case UnreadableFile::Mechanism::NoReadPermission:
        QVERIFY(!QFileInfo::exists(path));
        break;
    case UnreadableFile::Mechanism::LockedWithoutSharing:
        QVERIFY(QFileInfo(path).isFile());
        QVERIFY(unreadable.release());
        restart();
        QVERIFY(!QFileInfo::exists(path));
        break;
    case UnreadableFile::Mechanism::Directory:
        QVERIFY(QFileInfo(path).isDir());
        restart();
        QVERIFY(QFileInfo(path).isDir());
        break;
    }
    for (const QString &name : calculationRecordFiles())
        QVERIFY2(!name.startsWith(stem), qPrintable(name));
}

void ResultStoreTest::registryChangeDeletesRecord_data()
{
    QTest::addColumn<QString>("change");
    for (const char *change : {"providerRegistered", "familyRegistered", "providerUnregistered",
                               "candidateBehindProviderRegistered", "teardownRemoval"})
        QTest::newRow(change) << QString::fromLatin1(change);
}

// A registry change made while the application runs that changes what a name
// a result looked up resolves to drops it and deletes its record at once, like
// an input change. Here EA_IN, which expA looks up and expB reads through
// expA, is provided by a calculation (test.store.shadow, EA_IN = 4) after a
// candidate that reads the missing EA_SRC (test.store.fromSrc, passed over):
// a calculation or a family that now provides EA_SRC hands EA_IN to fromSrc,
// and removing shadow leaves EA_IN to nothing. A candidate registered behind
// shadow is never tried: nothing is dropped or deleted. A teardown removal
// drops the results silently and deletes nothing: the records restore at the
// next load, once shadow is registered again (as at the next start).
void ResultStoreTest::registryChangeDeletesRecord()
{
    QFETCH(QString, change);
    const CalculationDescriptor shadow = constantCalculation(kShadow, QStringLiteral("EA_IN"), 4);
    CalculationDescriptor fromSrc;
    fromSrc.id = kFromSrc;
    fromSrc.inputs = {CalcInput::attribute(QStringLiteral("EA_SRC"))};
    fromSrc.outputs = {attrKey("EA_IN")};
    fromSrc.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute(QStringLiteral("EA_IN"), ctx.attribute(QStringLiteral("EA_SRC")));
    };
    CalculationFamily famSrc;
    famSrc.id = kFamilySrc;
    famSrc.policy = EvaluationPolicy::OnDemand;
    famSrc.instantiate = [](const DependencyKey &name) -> std::optional<CalculationDescriptor> {
        if (!(name == DependencyKey::attribute(QStringLiteral("EA_SRC"))))
            return std::nullopt;
        CalculationDescriptor d;
        d.id = QStringLiteral("k");     // instance key
        d.outputs = {name};
        d.compute = [](const EvaluationContext &) {
            return CalculationResult().setAttribute(QStringLiteral("EA_SRC"), 7);
        };
        return d;
    };

    QVERIFY(m_extra->add(fromSrc));
    QVERIFY(m_extra->add(shadow));
    m_model->flushPendingInvalidations();
    QVERIFY(setInput("s1", "EB_IN", 10));
    QCOMPARE(engine("s1").request(kExpA).status, ResultStatus::Ok);
    QCOMPARE(engine("s1").request(kExpB).status, ResultStatus::Ok);
    QCOMPARE(session("s1").getAttribute(QStringLiteral("EA1")), QVariant(5));     // EA_IN from shadow
    QVERIFY(waitForIdle(*m_model));
    const QString pathA = recordPath("s1", QStringLiteral("exp%41"));
    const QString pathB = recordPath("s1", QStringLiteral("exp%42"));
    const QByteArray bytesA = bytesOf(pathA);
    const QByteArray bytesB = bytesOf(pathB);
    QVERIFY(!bytesA.isEmpty());
    QVERIFY(!bytesB.isEmpty());
    m_model->resetStoredResultStats();

    const bool keeps = change == QLatin1String("candidateBehindProviderRegistered");
    const bool deletes = !keeps && change != QLatin1String("teardownRemoval");
    {
        const Quiet quiet(*m_queue);
        if (change == QLatin1String("providerRegistered"))
            QVERIFY(m_extra->add(constantCalculation(kSrc, QStringLiteral("EA_SRC"), 7)));
        else if (change == QLatin1String("familyRegistered"))
            QVERIFY(m_extra->addFamily(famSrc));
        else if (change == QLatin1String("providerUnregistered"))
            QVERIFY(m_extra->remove(kShadow));
        else if (keeps)
            QVERIFY(m_extra->add(constantCalculation(kBehind, QStringLiteral("EA_IN"), 0)));
        else if (change == QLatin1String("teardownRemoval"))
            QVERIFY(m_extra->remove(kShadow, CalculationRegistry::Removal::Teardown));
        else
            QFAIL("unknown change");

        // No event-loop pass in between
        CalculationEngine &loaded = engine("s1");
        if (deletes) {
            QVERIFY(!QFileInfo::exists(pathA));
            QVERIFY(!QFileInfo::exists(pathB));
            QCOMPARE(stats().droppedRecordsDeleted, 2);
            QVERIFY(loaded.resultStatus(kExpB) != std::optional<ResultStatus>(ResultStatus::Ok));
        } else {
            QCOMPARE(bytesOf(pathA), bytesA);
            QCOMPARE(bytesOf(pathB), bytesB);
            QCOMPARE(stats().droppedRecordsDeleted, 0);
        }
        if (keeps) {
            QCOMPARE(loaded.resultStatus(kExpA), std::optional<ResultStatus>(ResultStatus::Ok));
            QCOMPARE(loaded.resultStatus(kExpB), std::optional<ResultStatus>(ResultStatus::Ok));
        } else {
            QVERIFY(loaded.resultStatus(kExpA) != std::optional<ResultStatus>(ResultStatus::Ok));
        }
        QVERIFY(quiet.holds());
    }

    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(evict({"s1"}), QString());
    if (change == QLatin1String("teardownRemoval")) {
        QVERIFY(m_extra->add(shadow));
        m_model->flushPendingInvalidations();
    }
    m_model->resetStoredResultStats();
    CalculationEngine &loaded = engine("s1");
    if (deletes) {
        QCOMPARE(stats().recordListings, 0);
        QCOMPARE(stats().recordsRead, 0);
        QVERIFY(loaded.resultStatus(kExpA) != std::optional<ResultStatus>(ResultStatus::Ok));
    } else {
        QCOMPARE(stats().recordsRestored, 2);
        QCOMPARE(loaded.resultStatus(kExpA), std::optional<ResultStatus>(ResultStatus::Ok));
        QCOMPARE(loaded.runCount(kExpA), 0);
    }
}

void ResultStoreTest::recordSurvivesUnrelatedChanges_data()
{
    QTest::addColumn<QString>("change");
    QTest::newRow("unrelatedRegistration") << QStringLiteral("unrelatedRegistration");
    QTest::newRow("declaredPreference") << QStringLiteral("declaredPreference");
}

// A registration of a name the result never looked up, or a declared
// preference it never read, changes the calculation environment (of the names
// it concerns) and never makes the record stale: it is restored at the next load and after a
// restart, with the change still in effect.
void ResultStoreTest::recordSurvivesUnrelatedChanges()
{
    QFETCH(QString, change);
    QVERIFY(setInput("s1", "EA_IN", 4));
    QCOMPARE(engine("s1").request(kExpA).status, ResultStatus::Ok);
    QVERIFY(waitForIdle(*m_model));
    const QString path = recordPath("s1", QStringLiteral("exp%41"));
    const QByteArray r0 = bytesOf(path);
    QVERIFY(!r0.isEmpty());
    const QList<DependencyKey> concerned = change == QLatin1String("unrelatedRegistration")
        ? QList<DependencyKey>{DependencyKey::attribute(QStringLiteral("_STORE_EXTRA"))}
        : QList<DependencyKey>{DependencyKey::attribute(QStringLiteral("_EXIT_TIME"))};
    const QString env0 = calculationEnvironmentDigest(concerned);
    const std::optional<CalculationRecord> record = storedRecord("s1", kExpA);
    QVERIFY(record.has_value());
    QVERIFY(!record->result.leaves.contains(GraphNode::preference(PreferenceKeys::ImportDescentPauseSeconds)));
    QCOMPARE(evict({"s1"}), QString());

    if (change == QLatin1String("unrelatedRegistration"))
        QVERIFY(m_extra->add(constantCalculation(kExtra, QStringLiteral("_STORE_EXTRA"), 1)));
    else if (change == QLatin1String("declaredPreference"))
        PreferencesManager::instance().setValue(PreferenceKeys::ImportDescentPauseSeconds, 45.0);
    else
        QFAIL("unknown change");
    m_model->flushPendingInvalidations();
    QVERIFY(calculationEnvironmentDigest(concerned) != env0);

    for (const bool afterRestart : {false, true}) {
        if (afterRestart)
            restart();
        m_model->resetStoredResultStats();
        const Quiet quiet(*m_queue);
        CalculationEngine &loaded = engine("s1");
        QCOMPARE(loaded.resultStatus(kExpA), std::optional<ResultStatus>(ResultStatus::Ok));
        QCOMPARE(loaded.runCount(kExpA), 0);
        QCOMPARE(stats().recordsRestored, 1);
        QCOMPARE(stats().staleRecordsDeleted, 0);
        QCOMPARE(bytesOf(path), r0);
        QVERIFY(quiet.holds());
    }
}

// A record whose lookup resolves differently at load (a new candidate for the
// looked-up name, computing from the same inputs, tried first) is stale and
// deleted; a candidate tried after the one recorded changes nothing.
void ResultStoreTest::lookupResolvingDifferentlyDeletesRecord()
{
    const auto provider = [](const QString &id) {
        return plusOne(id, QStringLiteral("PROV_IN"), QStringLiteral("PROV_OUT"));
    };
    const QString provider0 = QStringLiteral("test.store.provider0");
    const QString provider1 = QStringLiteral("test.store.provider1");
    const QString provider2 = QStringLiteral("test.store.provider2");
    QVERIFY(m_extra->add(provider(provider1)));
    QVERIFY(m_extra->add(timesExplicit(kReader, QStringLiteral("PROV_OUT"), QStringLiteral("READ_OUT"), 10)));
    m_model->flushPendingInvalidations();

    QVERIFY(setInput("s1", "PROV_IN", 2));
    QCOMPARE(engine("s1").request(kReader).status, ResultStatus::Ok);
    QCOMPARE(session("s1").getAttribute(QStringLiteral("READ_OUT")), QVariant(30));
    const std::optional<CalculationRecord> record = storedRecord("s1", kReader);
    QVERIFY(record.has_value());
    QVERIFY(hasResolution(record->result.resolutions, attrKey("PROV_OUT"), StoredResolution::Provider::Calculation,
                          provider1, QString()));
    QVERIFY(hasResolution(record->result.resolutions, attrKey("PROV_IN"), StoredResolution::Provider::SessionData));
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(evict({"s1"}), QString());
    const QString path = recordPath("s1", QStringLiteral("test%2Estore%2Ereader"));
    const QByteArray r0 = bytesOf(path);
    QVERIFY(!r0.isEmpty());

    // Control: a losing candidate (registered after provider1, never tried first)
    QVERIFY(m_extra->add(provider(provider2)));
    m_model->resetStoredResultStats();
    {
        CalculationEngine &loaded = engine("s1");
        QCOMPARE(stats().recordsRestored, 1);
        QCOMPARE(loaded.runCount(kReader), 0);
        QCOMPARE(session("s1").getAttribute(QStringLiteral("READ_OUT")), QVariant(30));
    }
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(evict({"s1"}), QString());
    QCOMPARE(bytesOf(path), r0);

    // A winning candidate with the same inputs: re-registering goes to the end,
    // so provider0 is now tried before provider1 and provider2.
    QVERIFY(m_extra->add(provider(provider0)));
    QVERIFY(m_extra->remove(provider1));
    QVERIFY(m_extra->add(provider(provider1)));
    QVERIFY(m_extra->remove(provider2));
    QVERIFY(m_extra->add(provider(provider2)));
    m_model->resetStoredResultStats();
    const Quiet quiet(*m_queue);
    CalculationEngine &loaded = engine("s1");
    QVERIFY(!QFileInfo::exists(path));
    QCOMPARE(stats().staleRecordsDeleted, 1);
    QVERIFY(loaded.resultStatus(kReader) != std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(loaded.runCount(kReader), 0);
    QVERIFY(quiet.holds());

    // Which check failed
    CalculationRecord decoded;
    QCOMPARE(decodeCalculationRecord(r0, &decoded), CalculationRecordStatus::Ok);
    const CalculationEngine::RestoreOutcome outcome = loaded.restoreResult(decoded.result);
    QCOMPARE(outcome.kind, CalculationEngine::RestoreOutcome::Kind::Stale);
    QCOMPARE(outcome.staleCheck, CalculationEngine::RestoreOutcome::StaleCheck::Resolutions);
}

void ResultStoreTest::pluginEditStalesRecordsThatReadIt_data()
{
    QTest::addColumn<QString>("ingredients");
    QTest::addColumn<bool>("stale");

    QTest::newRow("unchanged") << QStringLiteral("unchanged") << false;
    QTest::newRow("editedFile") << QStringLiteral("editedFile") << true;
    QTest::newRow("addedFile") << QStringLiteral("addedFile") << true;
    QTest::newRow("pythonVersion") << QStringLiteral("pythonVersion") << true;
    QTest::newRow("numpyVersion") << QStringLiteral("numpyVersion") << true;
}

// A stand-in plug-in calculation declares the plug-in code identity as its
// result version. Between runs, any change of the ingredients makes the
// record of a requested calculation that read its output stale; a requested
// calculation that read no plug-in output is restored in every row.
void ResultStoreTest::pluginEditStalesRecordsThatReadIt()
{
    QFETCH(QString, ingredients);
    QFETCH(bool, stale);
    const QString v1 = pluginCodeIdentity(pluginIngredients(QStringLiteral("v1")));
    const QString changed = pluginCodeIdentity(pluginIngredients(ingredients));
    QCOMPARE(changed != v1, stale);
    const auto plugin = [](const QString &identity) {
        CalculationDescriptor d = plusOne(kPlugin, QStringLiteral("PL_IN"), QStringLiteral("PL_OUT"));
        d.resultVersion = identity;
        return d;
    };

    QVERIFY(m_extra->add(plugin(v1)));
    QVERIFY(m_extra->add(timesExplicit(kReadsPlugin, QStringLiteral("PL_OUT"), QStringLiteral("RP_OUT"), 2)));
    QVERIFY(m_extra->add(timesExplicit(kReadsNoPlugin, QStringLiteral("NP_IN"), QStringLiteral("RN_OUT"), 2)));
    m_model->flushPendingInvalidations();

    QVERIFY(setInput("s1", "PL_IN", 1));
    QVERIFY(setInput("s1", "NP_IN", 3));
    QCOMPARE(engine("s1").request(kReadsPlugin).status, ResultStatus::Ok);
    QCOMPARE(engine("s1").request(kReadsNoPlugin).status, ResultStatus::Ok);
    QCOMPARE(session("s1").getAttribute(QStringLiteral("RP_OUT")), QVariant(4));
    QCOMPARE(session("s1").getAttribute(QStringLiteral("RN_OUT")), QVariant(6));

    const std::optional<CalculationRecord> readsPlugin = storedRecord("s1", kReadsPlugin);
    const std::optional<CalculationRecord> readsNoPlugin = storedRecord("s1", kReadsNoPlugin);
    QVERIFY(readsPlugin.has_value());
    QVERIFY(readsNoPlugin.has_value());
    QVERIFY(hasResolution(readsPlugin->result.resolutions, attrKey("PL_OUT"), StoredResolution::Provider::Calculation,
                          kPlugin, v1));
    for (const StoredResolution &r : readsNoPlugin->result.resolutions)
        QVERIFY(r.provider != StoredResolution::Provider::Calculation);
    QVERIFY(waitForIdle(*m_model));
    const QString pluginPath = recordPath("s1", QStringLiteral("test%2Estore%2Ereads%50lugin"));
    const QString noPluginPath = recordPath("s1", QStringLiteral("test%2Estore%2Ereads%4Eo%50lugin"));
    QVERIFY(QFileInfo(pluginPath).isFile());
    QVERIFY(QFileInfo(noPluginPath).isFile());

    // The next run loads the plug-ins with the row's ingredients
    bool swapped = false;
    restart([&] { swapped = m_extra->remove(kPlugin) && m_extra->add(plugin(changed)); });
    QVERIFY(swapped);
    m_model->resetStoredResultStats();
    const Quiet quiet(*m_queue);
    CalculationEngine &loaded = engine("s1");

    QCOMPARE(loaded.resultStatus(kReadsNoPlugin), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(session("s1").getAttribute(QStringLiteral("RN_OUT")), QVariant(6));
    QCOMPARE(loaded.runCount(kReadsNoPlugin), 0);
    QVERIFY(QFileInfo(noPluginPath).isFile());
    if (!stale) {
        QCOMPARE(loaded.resultStatus(kReadsPlugin), std::optional<ResultStatus>(ResultStatus::Ok));
        QCOMPARE(session("s1").getAttribute(QStringLiteral("RP_OUT")), QVariant(4));
        QCOMPARE(stats().recordsRestored, 2);
        QCOMPARE(stats().staleRecordsDeleted, 0);
    } else {
        QVERIFY(!QFileInfo::exists(pluginPath));
        QVERIFY(loaded.resultStatus(kReadsPlugin) != std::optional<ResultStatus>(ResultStatus::Ok));
        QCOMPARE(stats().recordsRestored, 1);
        QCOMPARE(stats().staleRecordsDeleted, 1);
    }
    QCOMPARE(loaded.runCount(kReadsPlugin), 0);
    QVERIFY(quiet.holds());
}

// A result installed some other way wins, and its record stays. Reachable
// only by driving the store directly.
void ResultStoreTest::alreadyInstalledIsKept()
{
    QVERIFY(setInput("s1", "EA_IN", 4));
    QCOMPARE(engine("s1").request(kExpA).status, ResultStatus::Ok);
    QVERIFY(waitForIdle(*m_model));
    const QString path = recordPath("s1", QStringLiteral("exp%41"));
    const QByteArray bytes = bytesOf(path);
    QVERIFY(!bytes.isEmpty());

    std::optional<SessionData> copy = LogbookManager::instance().loadSession("s1");
    QVERIFY(copy.has_value());
    CalculationEngine &copyEngine = copy->calculationEngine();
    QCOMPARE(copyEngine.request(kExpA).status, ResultStatus::Ok);
    QCOMPARE(copyEngine.runCount(kExpA), 1);

    CalculationResultStore store;
    CalculationResultStore::RestoreSummary summary = store.restoreSession("s1", copyEngine);
    QCOMPARE(summary.kept, 1);
    QCOMPARE(summary.restored, 0);
    QCOMPARE(summary.deleted, 0);
    QCOMPARE(bytesOf(path), bytes);
    QCOMPARE(copyEngine.runCount(kExpA), 1);
    QCOMPARE(copyEngine.preparedCount(), 0);

    // Also when the installed result is not Ok
    copy->removeAttribute(QStringLiteral("EA_IN"));
    QCOMPARE(copyEngine.request(kExpA).status, ResultStatus::MissingInput);
    summary = store.restoreSession("s1", copyEngine);
    QCOMPARE(summary.kept, 1);
    QCOMPARE(summary.restored, 0);
    QCOMPARE(summary.deleted, 0);
    QCOMPARE(bytesOf(path), bytes);
    QCOMPARE(copyEngine.runCount(kExpA), 1);
    QCOMPARE(copyEngine.preparedCount(), 0);
    QCOMPARE(store.stats().recordsKept, 2);
    QCOMPARE(store.stats().restoreCalls, 2);
}

// The column worker's and the bulk edit's temporary loads never read a
// record: even a stale one stays until a real load.
void ResultStoreTest::temporaryLoadsNeverRestore()
{
    QVERIFY(setInput("s1", "EA_IN", 4));
    QCOMPARE(engine("s1").request(kExpA).status, ResultStatus::Ok);
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(rewriteRecord("s1", kExpA, [](CalculationRecord &r) { r.calculationCompatibility += 1; }), QString());
    const QString path = recordPath("s1", QStringLiteral("exp%41"));
    const QByteArray bytes = bytesOf(path);
    QVERIFY(!bytes.isEmpty());
    QCOMPARE(evict({"s1"}), QString());
    m_model->resetStoredResultStats();
    m_model->resetColumnWorkStats();

    // The column worker
    LogbookColumnStore::instance().setColumns({descriptionColumn(), exitTimeColumn()});
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(m_model->columnWorkStats().sessionsLoaded >= 1);
    QVERIFY(!isLoaded("s1"));

    // The bulk edit on a stub
    m_model->startBulkEdit({row("s1")}, 0, QStringLiteral("bulk"));
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(!isLoaded("s1"));

    QCOMPARE(stats().restoreCalls, 0);
    QCOMPARE(stats().recordsRead, 0);
    QCOMPARE(bytesOf(path), bytes);

    // A real load reads it, and finds it stale
    session("s1");
    QCOMPARE(stats().staleRecordsDeleted, 1);
    QVERIFY(!QFileInfo::exists(path));
}

// The main window's delete sequence removes the session's records, and only its own.
void ResultStoreTest::deletingSessionRemovesRecords()
{
    QVERIFY(setInput("s1", "EA_IN", 4));
    QVERIFY(setInput("s2", "EA_IN", 4));
    QCOMPARE(engine("s1").request(kExpA).status, ResultStatus::Ok);
    QCOMPARE(engine("s2").request(kExpA).status, ResultStatus::Ok);
    QVERIFY(waitForIdle(*m_model));
    const QString csv1 = sessionFilePath("s1");
    const QString record1 = recordPath("s1", QStringLiteral("exp%41"));
    const QString record2 = recordPath("s2", QStringLiteral("exp%41"));
    const QByteArray bytes2 = bytesOf(record2);
    QVERIFY(QFileInfo(csv1).isFile());
    QVERIFY(QFileInfo(record1).isFile());
    QVERIFY(!bytes2.isEmpty());

    LogbookManager &logbook = LogbookManager::instance();
    QVERIFY(m_model->removeSessions({"s1"}));
    QVERIFY(logbook.removeSession("s1"));
    QVERIFY(logbook.flushIndex());

    QVERIFY(!QFileInfo::exists(csv1));
    QVERIFY(!QFileInfo::exists(record1));
    QCOMPARE(bytesOf(record2), bytes2);
}

// A record whose session file is gone is removed by the next start's scan.
void ResultStoreTest::strayRecordRemovedAtRestart()
{
    QVERIFY(setInput("s1", "EA_IN", 4));
    QCOMPARE(engine("s1").request(kExpA).status, ResultStatus::Ok);
    QVERIFY(waitForIdle(*m_model));
    const QString stem = sessionFileStem("s1");
    const QString csv = sessionFilePath("s1");
    const QString record = recordPath("s1", QStringLiteral("exp%41"));
    QVERIFY(QFileInfo(record).isFile());

    m_queue->shutdown();
    m_queue.reset();
    m_model.reset();
    QVERIFY(QFile::remove(csv));
    restart();

    QVERIFY(!QFileInfo::exists(record));
    for (const QString &name : calculationRecordFiles())
        QVERIFY2(!name.startsWith(stem), qPrintable(name));
}

FLYSIGHT_TEST_MAIN(ResultStoreTest)
#include "tst_result_store.moc"
