// Stored fusion results end to end (store-requested-calculations, spec section
// 8 except the logbook-column item): SessionModel + JobQueue + PlotModel +
// PlotRequests + Fusion::registerFusionCalculations on a real temporary
// logbook, with real fits.
//
//  - a fit survives unload and restart bit for bit (the seventeen channels,
//    the derived values, the diagnostics and the detail equal the fresh
//    publish, and the goldens), with no job, no run and no refresh count;
//  - a rejection and a solver failure come back with their reason and badge;
//  - validity follows the inputs (an unrelated edit keeps the record, a
//    dependency edit or an IMU merge drops it) and the code stamps;
//  - a fit published before the session's first save is stored and restored;
//  - the session file's bytes never depend on a record.
//
// The oracle (verifyAgainstFresh / evaluateFresh) is never used on a session
// with the fit installed: it would run the fit again. Expected values are
// literals and the committed goldens of the kernel.

#include <functional>
#include <memory>
#include <optional>

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QtTest>

#include "calculationrecord.h"
#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "fusion/fusionregistration.h"
#include "fusionfixtures.h"
#include "fusiongolden.h"
#include "fusionsessions.h"
#include "jobfixture.h"
#include "jobmodel.h"
#include "jobqueue.h"
#include "logbookcolumn.h"
#include "logbookmanager.h"
#include "logbookprobe.h"
#include "plotfixture.h"
#include "plotmodel.h"
#include "plotrequests.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "sessiondata.h"
#include "sessionmodel.h"
#include "testenvironment.h"
#include "testmain.h"
#include "testutil.h"

using namespace FlySight;
using namespace FlySightTest;

using Kind = JobQueue::RequestResult::Kind;
using Control = PlotRowState::Control;

Q_DECLARE_METATYPE(FlySight::DependencyKey)

namespace {

const QString kFit = QString::fromLatin1(Fusion::FitCalculationId);     // "builtin.fusion.fit"
const QString kRoll = QStringLiteral("Fusion/roll");
const QString kDiagnostics = QStringLiteral("_FUSION_DIAGNOSTICS");
const QString kRecordSuffix = QStringLiteral(".builtin%2Efusion%2Efit.fvresult");
const QString kExtra = QStringLiteral("test.store.extra");

constexpr int kFitTimeoutMs = 120000;

// The reason of tst_fusion_kernel::nonConvergenceIsSolverFailure, and a
// diagnostics string of the shape the kernel writes for it.
const QString kSolverFailureReason =
    QStringLiteral("Batch fusion did not converge (iteration limit); sensor fusion unavailable");
const QString kSolverFailureDiagnostics = QStringLiteral(
    "{\"algorithm\":\"batch-temperature-bias-v3\","
    "\"failure\":\"Batch fusion did not converge (iteration limit); sensor fusion unavailable\"}");

/// What a fresh publish showed, for the bit-for-bit comparison with a restore.
struct FitValues {
    QHash<QString, QVector<double>> channels;   ///< fusionMeasurementNames(), accH, _system_time
    QString diagnostics;
    QString detail;
};

/// The bytes of a file; a null QByteArray when it does not exist or is not a file.
QByteArray bytesOf(const QString &path)
{
    QFile file(path);
    if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

/// What a sessionLoaded watcher saw for one session.
struct LoadWatch {
    bool seen = false;
    std::optional<ResultStatus> status;
};

bool isNotRequested(const std::optional<ResultStatus> &status)
{
    return !status.has_value() || *status == ResultStatus::NotRequested;
}

} // namespace

class FusionStoreTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void restoredAfterEvictionIsBitIdentical();
    void restoredAfterRestartIsBitIdentical();
    void restoredRejectionShowsBadge();
    void restoredSolverFailureShowsBadge();
    void unrelatedEditKeepsRecord();
    void dependencyEditDropsRecord_data();
    void dependencyEditDropsRecord();
    void mergeIntoLoadedSessionDropsRecord();
    void mergeIntoUnloadedSession_data();
    void mergeIntoUnloadedSession();
    void codeStampChangeDropsRecordOnLoad_data();
    void codeStampChangeDropsRecordOnLoad();
    void sessionFileBytesUnaffectedByRecord();
    void fittedBeforeFirstSaveIsRestored_data();
    void fittedBeforeFirstSaveIsRestored();

private:
    [[nodiscard]] QString addSessions(const QList<SessionData> &sessions)
    {
        return FlySightTest::addSessions(*m_model, sessions);
    }
    SessionData &session(const QString &id) { return m_model->sessionRef(m_model->getSessionRow(id)); }
    CalculationEngine &engine(const QString &id) { return session(id).calculationEngine(); }
    QVector<double> fusion(const QString &id, const QString &name)
    {
        return session(id).getMeasurement(QStringLiteral("Fusion"), name);
    }
    bool isAvailable(const QString &id, const DependencyKey &name)
    {
        if (name.type == DependencyKey::Type::Attribute)
            return session(id).getAttribute(name.attributeKey).isValid();
        return !session(id).getMeasurement(name.measurementKey.first, name.measurementKey.second).isEmpty();
    }
    /// The names of fusionNames() that are available in the session, as text.
    QString availableIn(const QString &id);
    bool isLoaded(const QString &id) const
    {
        const int r = m_model->getSessionRow(id);
        return r >= 0 && std::as_const(*m_model).rowAt(r).isLoaded();
    }
    void show(const QStringList &ids, bool visible = true) { PlotFixture::show(*m_model, ids, visible); }
    /// A programmatic check: never a gesture.
    void check(const QString &measurement, bool enabled = true)
    {
        m_plots->setPlotEnabled(QStringLiteral("Fusion"), measurement, enabled);
    }
    /// The current row state: a pending pass runs first.
    PlotRowState row(const QString &plotId)
    {
        m_requests->flush();
        return m_requests->rowState(plotId);
    }
    const CalculationResultStore::Stats &stats() const { return m_model->storedResultStats(); }

    /// sessions/<stem>.builtin%2Efusion%2Efit.fvresult; the stem comes from
    /// index.json on disk.
    static QString recordPath(const QString &id)
    {
        return TestEnvironment::instance().sessionsDir() + QLatin1Char('/') + sessionFileStem(id) + kRecordSuffix;
    }

    FitValues capture(const QString &id);
    /// Empty when every channel has the same bits, the diagnostics the same
    /// UTF-8 bytes and the detail the same text as `fresh`; else the first difference.
    QString differenceFrom(const FitValues &fresh, const QString &id);
    /// Hides the row, evicts it (capacity 0), checks it is a stub and that its
    /// record kept its bytes, sets the capacity back to 50 and shows the row
    /// again. Empty when all of that held.
    [[nodiscard]] QString unloadAndReload(const QString &id);
    /// A simulated application restart: new logbook state, a model of stubs
    /// from the index, a new queue, plot model (unchecked) and request component.
    void restart();
    /// Reads the record, applies `mutate`, writes it back. Empty on success.
    [[nodiscard]] QString rewriteRecord(const QString &id, const std::function<void(CalculationRecord &)> &mutate);
    void watchLoad(QObject *scope, const QString &id, LoadWatch *out);
    /// Empty when the blocker reports agree in state, blockers and notes.
    static QString reportDifference(const BlockerReport &got, const BlockerReport &expected);

    std::unique_ptr<SessionModel> m_model;
    std::unique_ptr<JobQueue> m_queue;
    std::unique_ptr<PlotModel> m_plots;
    std::unique_ptr<PlotRequests> m_requests;
    QStringList m_registryBefore;
};

void FusionStoreTest::initTestCase()
{
    // As the application does: every registration before the logbook and the
    // model exist (a live model reacts to registry changes).
    TestEnvironment::instance().registerBuiltIns();
    registerFusionOnce();

    // One logbook column that reads stored data only
    PreferencesManager::instance().registerPreference(PreferenceKeys::LogbookColumnsVersion, 0);
    LogbookColumnStore::instance().setColumns({descriptionColumn()});

    qRegisterMetaType<DependencyKey>();
}

void FusionStoreTest::init()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();
    env.resetPreferencesToDefaults();
    LogbookManager::instance().initialize();
    m_registryBefore = CalculationRegistry::instance().registeredIds();

    m_model = std::make_unique<SessionModel>();
    m_queue = std::make_unique<JobQueue>(m_model.get());
    m_plots = std::make_unique<PlotModel>();
    m_plots->setPlots(fusionPlots());
    m_requests = std::make_unique<PlotRequests>(m_model.get(), m_plots.get(), m_queue.get());
}

// Note what is to be checked, tear everything down, and only then check (see tst_jobqueue).
void FusionStoreTest::cleanup()
{
    if (m_queue)
        m_queue->shutdown();
    QStringList stillPinned;
    if (m_model) {
        for (int r = 0; r < m_model->rowCount(); ++r) {
            const QString id = std::as_const(*m_model).rowAt(r).sessionId;
            if (m_model->isSessionPinned(id))
                stillPinned.append(id);
        }
    }

    // Reverse order of construction
    m_requests.reset();
    m_plots.reset();
    m_queue.reset();
    m_model.reset();
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);

    QCOMPARE(stillPinned, QStringList());
    QCOMPARE(CalculationRegistry::instance().registeredIds(), m_registryBefore);
    QCOMPARE(CalculationRegistry::instance().enrolledEngineCount(), 0);
}

QString FusionStoreTest::availableIn(const QString &id)
{
    QStringList available;
    for (const DependencyKey &name : fusionNames()) {
        if (isAvailable(id, name))
            available.append(name.type == DependencyKey::Type::Attribute ? name.attributeKey
                                                                         : name.measurementKey.second);
    }
    return available.join(QStringLiteral(", "));
}

FitValues FusionStoreTest::capture(const QString &id)
{
    FitValues values;
    QStringList names = fusionMeasurementNames();
    names << QStringLiteral("accH") << QStringLiteral("_system_time");
    for (const QString &name : std::as_const(names))
        values.channels.insert(name, fusion(id, name));
    values.diagnostics = session(id).getAttribute(kDiagnostics).toString();
    values.detail = engine(id).resultDetail(kFit);
    return values;
}

QString FusionStoreTest::differenceFrom(const FitValues &fresh, const QString &id)
{
    const FitValues now = capture(id);
    QStringList names = fresh.channels.keys();
    names.sort();
    for (const QString &name : std::as_const(names)) {
        if (!sameBitsEverywhere(now.channels.value(name), fresh.channels.value(name)))
            return QStringLiteral("Fusion/%1 differs (%2 samples, fresh %3)")
                .arg(name).arg(now.channels.value(name).size()).arg(fresh.channels.value(name).size());
    }
    if (now.diagnostics.toUtf8() != fresh.diagnostics.toUtf8())
        return QStringLiteral("the diagnostics differ");
    if (now.detail != fresh.detail)
        return QStringLiteral("the detail differs: '%1' != '%2'").arg(now.detail, fresh.detail);
    return QString();
}

QString FusionStoreTest::unloadAndReload(const QString &id)
{
    const QString path = recordPath(id);
    const QByteArray before = bytesOf(path);
    show({id}, false);
    session(id);        // in the LRU list (a row that was never touched is not)
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 0);
    const bool stub = !isLoaded(id);
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    if (!stub)
        return id + QStringLiteral(" was not evicted");
    if (bytesOf(path) != before)
        return QStringLiteral("eviction changed the record");
    show({id});
    if (!isLoaded(id))
        return id + QStringLiteral(" was not loaded again");
    return QString();
}

void FusionStoreTest::restart()
{
    if (m_queue)
        m_queue->shutdown();
    m_requests.reset();
    m_plots.reset();
    m_queue.reset();
    m_model.reset();

    LogbookManager &logbook = LogbookManager::instance();
    TestEnvironment::instance().reopenLogbook();
    logbook.initialize();

    m_model = std::make_unique<SessionModel>();
    m_model->populateFromIndex(logbook.cachedColumnValues(LogbookColumnStore::instance().enabledColumns()),
                               logbook.lastAccessedMap());
    m_queue = std::make_unique<JobQueue>(m_model.get());
    m_plots = std::make_unique<PlotModel>();
    m_plots->setPlots(fusionPlots());
    m_requests = std::make_unique<PlotRequests>(m_model.get(), m_plots.get(), m_queue.get());
}

QString FusionStoreTest::rewriteRecord(const QString &id, const std::function<void(CalculationRecord &)> &mutate)
{
    LogbookManager &logbook = LogbookManager::instance();
    const CalculationRecordRead read = logbook.readCalculationRecord(id, kFit);
    if (read.status != CalculationRecordStatus::Ok)
        return QStringLiteral("the record could not be read: ") + read.error;
    CalculationRecord record = *read.record;
    mutate(record);
    QString error;
    if (!logbook.writeCalculationRecord(id, record, &error))
        return QStringLiteral("the record could not be written: ") + error;
    return QString();
}

void FusionStoreTest::watchLoad(QObject *scope, const QString &id, LoadWatch *out)
{
    SessionModel *model = m_model.get();
    connect(model, &SessionModel::sessionLoaded, scope, [model, id, out](const QString &loadedId) {
        if (loadedId != id)
            return;
        const auto guard = model->stableRows();
        const SessionData *loaded = model->loadedSession(id);
        out->seen = true;
        out->status = loaded ? loaded->calculationEngine().resultStatus(kFit) : std::nullopt;
    });
}

QString FusionStoreTest::reportDifference(const BlockerReport &got, const BlockerReport &expected)
{
    if (got.state != expected.state)
        return QStringLiteral("state %1 != %2").arg(int(got.state)).arg(int(expected.state));
    QStringList gotIds, expectedIds;
    for (const CalculationBlocker &b : got.blockers)
        gotIds.append(b.instanceId);
    for (const CalculationBlocker &b : expected.blockers)
        expectedIds.append(b.instanceId);
    if (gotIds != expectedIds)
        return QStringLiteral("blockers %1 != %2").arg(gotIds.join(','), expectedIds.join(','));
    if (got.notProduced.size() != expected.notProduced.size())
        return QStringLiteral("%1 notes != %2").arg(got.notProduced.size()).arg(expected.notProduced.size());
    for (qsizetype i = 0; i < got.notProduced.size(); ++i) {
        const UnproducedNote &a = got.notProduced.at(i);
        const UnproducedNote &b = expected.notProduced.at(i);
        if (a.calculation.instanceId != b.calculation.instanceId || a.status != b.status || a.detail != b.detail)
            return QStringLiteral("note %1 differs: %2 / %3").arg(i).arg(a.calculation.instanceId, a.detail);
    }
    return QString();
}

// ---- Bit-identical restores ------------------------------------------------------------

// Spec 8: fitted, saved, unloaded, reloaded: the goldens, no job, no refresh count.
void FusionStoreTest::restoredAfterEvictionIsBitIdentical()
{
    const auto restoreCapacity = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    });
    QCOMPARE(addSessions({fixtureSession(QStringLiteral("coarse_maneuver"), QStringLiteral("a"))}), QString());
    show({"a"});
    check(QStringLiteral("roll"));
    QCOMPARE(m_requests->plotCheckedByUser(kRoll), 1);
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(m_queue->model()->record(0).state, JobState::Succeeded);
    QVERIFY(waitForIdle(*m_model));
    const QString path = recordPath("a");
    QVERIFY(QFileInfo(path).isFile());
    QCOMPARE(stats().recordsWritten, 1);
    const qint64 writeNanoseconds = stats().writeNanoseconds;

    const FitValues fresh = capture("a");
    const QVariant rollAtExit = session("a").getAttribute(fusionRollAtExit());
    QVERIFY(rollAtExit.isValid());
    const QByteArray r0 = bytesOf(path);
    const QSet<GraphNode> dependencies = engine("a").dependenciesOf(GraphNode::result(kFit));
    const BlockerReport freshBlockers = engine("a").blockers(fusionKey(QStringLiteral("roll")));
    QCOMPARE(freshBlockers.state, BlockerReport::State::Available);

    m_model->resetStoredResultStats();
    QObject scope;
    LoadWatch atLoad;
    watchLoad(&scope, "a", &atLoad);
    const Quiet quiet(*m_queue);
    QElapsedTimer timer;
    timer.start();
    QCOMPARE(unloadAndReload("a"), QString());
    const qint64 reloadNanoseconds = timer.nsecsElapsed();

    QString difference = differenceFrom(fresh, "a");
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    QVERIFY(sameBits(session("a").getAttribute(fusionRollAtExit()).toDouble(), rollAtExit.toDouble()));
    const FusionGolden golden = loadFusionGolden(QStringLiteral("coarse_maneuver"));
    difference = goldenDifference(session("a"), golden);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    const QJsonObject diagnostics =
        QJsonDocument::fromJson(session("a").getAttribute(kDiagnostics).toString().toUtf8()).object();
    difference = compareJson(QStringLiteral("diagnostics"), diagnostics, golden.diagnostics);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));

    QCOMPARE(engine("a").runCount(kFit), 0);
    QCOMPARE(engine("a").preparedCount(), 0);
    QCOMPARE(engine("a").readiness(kFit).state, CalculationReadiness::State::Done);
    QCOMPARE(m_queue->request("a", kFit).kind, Kind::NothingToDo);
    QCOMPARE(m_queue->model()->rowCount(), 1);      // only the first job
    QVERIFY(quiet.holds());

    const PlotRowState state = row(kRoll);
    QVERIFY(state.isPlain());
    QCOMPARE(state.controlCount(), 0);
    QCOMPARE(engine("a").blockers(fusionKey(QStringLiteral("roll"))).state, BlockerReport::State::Available);
    QCOMPARE(engine("a").dependenciesOf(GraphNode::result(kFit)), dependencies);

    QVERIFY(atLoad.seen);
    QCOMPARE(atLoad.status, std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(bytesOf(path), r0);
    QCOMPARE(stats().recordsRestored, 1);
    QCOMPARE(stats().recordsWritten, 0);

    // Restore cost (reported, not asserted)
    qInfo().noquote() << QStringLiteral("restore of coarse_maneuver: %1 ms of a %2 ms reload; record %3 bytes, "
                                        "session file %4 bytes; write %5 ms")
                             .arg(double(stats().restoreNanoseconds) / 1e6, 0, 'f', 3)
                             .arg(double(reloadNanoseconds) / 1e6, 0, 'f', 3)
                             .arg(r0.size())
                             .arg(QFileInfo(sessionFilePath("a")).size())
                             .arg(double(writeNanoseconds) / 1e6, 0, 'f', 3);
}

// Spec 8: the same after an application restart.
void FusionStoreTest::restoredAfterRestartIsBitIdentical()
{
    QCOMPARE(addSessions({fixtureSession(QStringLiteral("stationary_spin"), QStringLiteral("a"))}), QString());
    QCOMPARE(m_queue->request("a", kFit).kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));
    QCOMPARE(m_queue->model()->record(0).state, JobState::Succeeded);
    QVERIFY(waitForIdle(*m_model));
    const QString path = recordPath("a");
    QVERIFY(QFileInfo(path).isFile());

    const FitValues fresh = capture("a");
    const QVariant rollAtExit = session("a").getAttribute(fusionRollAtExit());
    QVERIFY(rollAtExit.isValid());
    const QByteArray r0 = bytesOf(path);
    const QSet<GraphNode> dependencies = engine("a").dependenciesOf(GraphNode::result(kFit));

    restart();
    check(QStringLiteral("roll"));
    QObject scope;
    LoadWatch atLoad;
    watchLoad(&scope, "a", &atLoad);
    const Quiet quiet(*m_queue);
    show({"a"});
    QVERIFY(isLoaded("a"));

    QString difference = differenceFrom(fresh, "a");
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    QVERIFY(sameBits(session("a").getAttribute(fusionRollAtExit()).toDouble(), rollAtExit.toDouble()));
    const FusionGolden golden = loadFusionGolden(QStringLiteral("stationary_spin"));
    difference = goldenDifference(session("a"), golden);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    const QJsonObject diagnostics =
        QJsonDocument::fromJson(session("a").getAttribute(kDiagnostics).toString().toUtf8()).object();
    difference = compareJson(QStringLiteral("diagnostics"), diagnostics, golden.diagnostics);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));

    QCOMPARE(engine("a").runCount(kFit), 0);
    QCOMPARE(engine("a").preparedCount(), 0);
    QCOMPARE(engine("a").readiness(kFit).state, CalculationReadiness::State::Done);
    QCOMPARE(m_queue->request("a", kFit).kind, Kind::NothingToDo);
    QCOMPARE(m_queue->model()->rowCount(), 0);
    QVERIFY(quiet.holds());

    const PlotRowState state = row(kRoll);
    QVERIFY(state.isPlain());
    QCOMPARE(state.controlCount(), 0);
    QCOMPARE(engine("a").blockers(fusionKey(QStringLiteral("roll"))).state, BlockerReport::State::Available);
    QCOMPARE(engine("a").dependenciesOf(GraphNode::result(kFit)), dependencies);

    QVERIFY(atLoad.seen);
    QCOMPARE(atLoad.status, std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(bytesOf(path), r0);
    QCOMPARE(stats().recordsRestored, 1);
    QCOMPARE(stats().recordsWritten, 0);
}

// ---- Failures keep their badge -----------------------------------------------------------

// Spec 8: a rejection is restored with its reason; the row shows the warning
// as today; no job runs.
void FusionStoreTest::restoredRejectionShowsBadge()
{
    const auto restoreCapacity = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    });
    QCOMPARE(addSessions({sessionFromFixture(fusionFixture(QStringLiteral("reject_origin")), QStringLiteral("r1"))}),
             QString());
    show({"r1"});
    check(QStringLiteral("roll"));
    QCOMPARE(m_requests->plotCheckedByUser(kRoll), 1);
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));
    QCOMPARE(m_queue->model()->record(0).state, JobState::Succeeded);    // a rejection is a result
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(QFileInfo(recordPath("r1")).isFile());
    const BlockerReport freshBlockers = engine("r1").blockers(fusionKey(QStringLiteral("roll")));
    const QString freshDiagnostics = session("r1").getAttribute(kDiagnostics).toString();
    QVERIFY(!freshDiagnostics.isEmpty());

    QCOMPARE(unloadAndReload("r1"), QString());

    const PlotRowState state = row(kRoll);
    QCOMPARE(state.failedCount, 1);
    QVERIFY(state.showsWarning());
    QCOMPARE(state.control(), Control::None);
    QCOMPARE(state.failed.at(0).reason,
             QStringLiteral("Sensor fusion: Local origin index outside GNSS samples"));

    const BlockerReport blockers = engine("r1").blockers(fusionKey(QStringLiteral("roll")));
    QCOMPARE(blockers.state, BlockerReport::State::NotProduced);
    const QString difference = reportDifference(blockers, freshBlockers);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    QCOMPARE(blockers.notProduced.size(), 1);
    QCOMPARE(blockers.notProduced.at(0).detail, QStringLiteral("Local origin index outside GNSS samples"));

    QCOMPARE(session("r1").getAttribute(kDiagnostics).toString().toUtf8(), freshDiagnostics.toUtf8());
    for (const QString &name : fusionMeasurementNames())
        QVERIFY2(fusion("r1", name).isEmpty(), qPrintable(name));

    {
        const Quiet quiet(*m_queue);
        QCOMPARE(m_requests->refreshPressed(kRoll), 0);
        QCOMPARE(m_requests->plotCheckedByUser(kRoll), 0);
        QVERIFY(quiet.holds());
    }
    QCOMPARE(engine("r1").runCount(kFit), 0);
    QCOMPARE(stats().recordsRestored, 1);
}

// Spec 8: a solver failure is restored with its reason. The registered fit
// cannot be driven into SolverFailed from a session fixture; the record is
// rewritten to the shape fusionregistration.cpp publishes for one (the same
// shape as a rejection), with the leaves and fingerprint of a real publish.
void FusionStoreTest::restoredSolverFailureShowsBadge()
{
    const auto restoreCapacity = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    });
    QCOMPARE(addSessions({sessionFromFixture(fusionFixture(QStringLiteral("reject_origin")), QStringLiteral("r1"))}),
             QString());
    show({"r1"});
    check(QStringLiteral("roll"));
    QCOMPARE(m_requests->plotCheckedByUser(kRoll), 1);
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(QFileInfo(recordPath("r1")).isFile());

    QCOMPARE(rewriteRecord("r1", [](CalculationRecord &record) {
                 CalculationResult bundle;
                 bundle.setAttribute(kDiagnostics, kSolverFailureDiagnostics);
                 bundle.setReason(kSolverFailureReason);
                 record.result.bundle = bundle;
                 record.result.detail = kSolverFailureReason;
             }), QString());

    const Quiet quiet(*m_queue);
    QCOMPARE(unloadAndReload("r1"), QString());

    const PlotRowState state = row(kRoll);
    QCOMPARE(state.failedCount, 1);
    QVERIFY(state.showsWarning());
    QCOMPARE(state.failed.at(0).reason, QStringLiteral("Sensor fusion: ") + kSolverFailureReason);

    const BlockerReport blockers = engine("r1").blockers(fusionKey(QStringLiteral("roll")));
    QCOMPARE(blockers.state, BlockerReport::State::NotProduced);
    QCOMPARE(blockers.notProduced.size(), 1);
    QCOMPARE(blockers.notProduced.at(0).detail, kSolverFailureReason);

    QCOMPARE(session("r1").getAttribute(kDiagnostics).toString().toUtf8(), kSolverFailureDiagnostics.toUtf8());
    QCOMPARE(engine("r1").runCount(kFit), 0);
    QVERIFY(quiet.holds());
}

// ---- Validity follows the inputs -------------------------------------------------------

// Spec 8: an edit the fit does not depend on keeps the record.
void FusionStoreTest::unrelatedEditKeepsRecord()
{
    const auto restoreCapacity = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    });
    QCOMPARE(addSessions({fixtureSession(QStringLiteral("coarse_linear"), QStringLiteral("a"))}), QString());
    QCOMPARE(m_queue->request("a", kFit).kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));
    QVERIFY(waitForIdle(*m_model));
    const QString path = recordPath("a");
    const QByteArray r0 = bytesOf(path);
    QVERIFY(!r0.isEmpty());
    const FitValues fresh = capture("a");
    m_model->resetStoredResultStats();

    QVERIFY(m_model->updateAttribute("a", QStringLiteral("_DESCRIPTION"), QStringLiteral("renamed")));
    QVERIFY(m_model->updateAttribute("a", QStringLiteral("_EXIT_TIME"), kFixtureExitTime + .25));
    QCOMPARE(bytesOf(path), r0);
    QCOMPARE(engine("a").resultStatus(kFit), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(engine("a").runCount(kFit), 1);
    QCOMPARE(stats().droppedRecordsDeleted, 0);

    QVERIFY(waitForIdle(*m_model));
    const Quiet quiet(*m_queue);
    QCOMPARE(unloadAndReload("a"), QString());
    QCOMPARE(engine("a").resultStatus(kFit), std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(engine("a").runCount(kFit), 0);
    QCOMPARE(stats().recordsRestored, 1);
    const QString difference = differenceFrom(fresh, "a");
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    QVERIFY(quiet.holds());
}

void FusionStoreTest::dependencyEditDropsRecord_data()
{
    QTest::addColumn<QString>("key");
    QTest::addColumn<QVariant>("value");

    // A declared input of the fit
    QTest::newRow("local origin index") << QStringLiteral("_LOCAL_ORIGIN_INDEX")
                                        << QVariant::fromValue(qlonglong(4));
    // Reached only through the schema conversion of the gyro (the fixture
    // records "2"; "1" is the other supported schema)
    QTest::newRow("SCHEMA_VER") << QStringLiteral("SCHEMA_VER") << QVariant(QStringLiteral("1"));
}

// Spec 8: changing a dependency (a declared input, or SCHEMA_VER, which the
// fit reaches through the gyro's schema conversion) drops the record at once;
// the calculation reads not requested and the row is refreshable exactly as
// today.
void FusionStoreTest::dependencyEditDropsRecord()
{
    QFETCH(QString, key);
    QFETCH(QVariant, value);
    const auto restoreCapacity = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    });
    QCOMPARE(addSessions({fixtureSession(QStringLiteral("coarse_maneuver"), QStringLiteral("a"))}), QString());
    show({"a"});
    check(QStringLiteral("roll"));
    QCOMPARE(m_requests->plotCheckedByUser(kRoll), 1);
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));
    QVERIFY(waitForIdle(*m_model));
    const QString path = recordPath("a");
    QVERIFY(QFileInfo(path).isFile());
    m_model->resetStoredResultStats();

    QVERIFY(session("a").getAttribute(key) != value);
    QVERIFY(m_model->updateAttribute("a", key, value));
    // No event-loop pass in between
    QVERIFY(!QFileInfo::exists(path));
    QCOMPARE(stats().droppedRecordsDeleted, 1);
    QVERIFY(isNotRequested(engine("a").resultStatus(kFit)));
    QVERIFY2(availableIn("a").isEmpty(), qPrintable(availableIn("a")));

    const PlotRowState state = row(kRoll);
    QCOMPARE(state.missingCount, 1);
    QCOMPARE(state.control(), Control::Refresh);
    QCOMPARE(state.controlCount(), 1);
    {
        const Quiet quiet(*m_queue);
        PlotFixture::spin(m_requests.get());
        QVERIFY(quiet.holds());
    }

    QVERIFY(waitForIdle(*m_model));
    m_model->resetStoredResultStats();
    QCOMPARE(unloadAndReload("a"), QString());
    QCOMPARE(stats().recordsRead, 0);
    QVERIFY(isNotRequested(engine("a").resultStatus(kFit)));
    QCOMPARE(engine("a").runCount(kFit), 0);
}

// Spec 8: merging IMU data into a loaded session drops the record.
void FusionStoreTest::mergeIntoLoadedSessionDropsRecord()
{
    QCOMPARE(addSessions({fixtureSession(QStringLiteral("coarse_linear"), QStringLiteral("a"))}), QString());
    QCOMPARE(m_queue->request("a", kFit).kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));
    QVERIFY(waitForIdle(*m_model));
    const QString path = recordPath("a");
    QVERIFY(QFileInfo(path).isFile());

    QVector<double> az = session("a").sourceMeasurement(QStringLiteral("IMU"), QStringLiteral("az"));
    const QString unit = session("a").sourceUnit(QStringLiteral("IMU"), QStringLiteral("az"));
    QVERIFY(!az.isEmpty());
    az[0] += 1e-3;
    SessionData incoming;
    incoming.setAttribute(QStringLiteral("SESSION_ID"), QStringLiteral("a"));
    incoming.setSourceMeasurement(QStringLiteral("IMU"), QStringLiteral("az"), az, unit);

    const int jobs = m_queue->model()->rowCount();
    const Quiet quiet(*m_queue);
    const QList<MergeResult> results = m_model->mergeSessions(QList<SessionData>{incoming});
    QCOMPARE(results.size(), 1);
    QCOMPARE(results.at(0).outcome, MergeResult::Outcome::Merged);
    QVERIFY(!QFileInfo::exists(path));
    QVERIFY(isNotRequested(engine("a").resultStatus(kFit)));
    QCOMPARE(m_queue->model()->rowCount(), jobs);
    QVERIFY(quiet.holds());
}

void FusionStoreTest::mergeIntoUnloadedSession_data()
{
    QTest::addColumn<bool>("imuData");
    QTest::newRow("unrelatedSensor") << false;
    QTest::newRow("imuData") << true;
}

// A merge into an unloaded session restores against the merged state: an
// unrelated sensor keeps the fit, IMU data makes the record stale.
void FusionStoreTest::mergeIntoUnloadedSession()
{
    QFETCH(bool, imuData);
    const auto restoreCapacity = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    });
    QCOMPARE(addSessions({fixtureSession(QStringLiteral("coarse_linear"), QStringLiteral("a"))}), QString());
    QCOMPARE(m_queue->request("a", kFit).kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));
    QVERIFY(waitForIdle(*m_model));
    const QString path = recordPath("a");
    const QByteArray r0 = bytesOf(path);
    QVERIFY(!r0.isEmpty());
    const FitValues fresh = capture("a");

    SessionData incoming;
    incoming.setAttribute(QStringLiteral("SESSION_ID"), QStringLiteral("a"));
    if (imuData) {
        QVector<double> az = session("a").sourceMeasurement(QStringLiteral("IMU"), QStringLiteral("az"));
        const QString unit = session("a").sourceUnit(QStringLiteral("IMU"), QStringLiteral("az"));
        az[0] += 1e-3;
        incoming.setSourceMeasurement(QStringLiteral("IMU"), QStringLiteral("az"), az, unit);
    } else {
        incoming.setSourceMeasurement(QStringLiteral("XTRA"), QStringLiteral("value"), {1, 2, 3}, QString());
    }

    // Hide and evict: the row really is a stub when the merge loads it. The
    // capacity goes back to 50 before the merge (loading nothing), or the
    // merge's own eviction pass would unload the merged row again.
    show({"a"}, false);
    session("a");
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 0);
    QVERIFY(!isLoaded("a"));
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    QVERIFY(!isLoaded("a"));
    QCOMPARE(bytesOf(path), r0);

    m_model->resetStoredResultStats();
    QObject scope;
    LoadWatch atLoad;
    watchLoad(&scope, "a", &atLoad);
    const Quiet quiet(*m_queue);
    const QList<MergeResult> results = m_model->mergeSessions(QList<SessionData>{incoming});
    QCOMPARE(results.size(), 1);
    QCOMPARE(results.at(0).outcome, MergeResult::Outcome::Merged);
    QVERIFY(isLoaded("a"));
    QVERIFY(atLoad.seen);

    if (!imuData) {
        QCOMPARE(atLoad.status, std::optional<ResultStatus>(ResultStatus::Ok));
        QCOMPARE(engine("a").resultStatus(kFit), std::optional<ResultStatus>(ResultStatus::Ok));
        QCOMPARE(engine("a").runCount(kFit), 0);
        QCOMPARE(stats().recordsRestored, 1);
        const QString difference = differenceFrom(fresh, "a");
        QVERIFY2(difference.isEmpty(), qPrintable(difference));
        QCOMPARE(bytesOf(path), r0);
    } else {
        QVERIFY(!QFileInfo::exists(path));
        QCOMPARE(stats().staleRecordsDeleted, 1);
        QCOMPARE(stats().recordsRestored, 0);
        QVERIFY(isNotRequested(engine("a").resultStatus(kFit)));
        QCOMPARE(engine("a").runCount(kFit), 0);

        // Which check failed: the same leaves, another fingerprint
        CalculationRecord record;
        QCOMPARE(decodeCalculationRecord(r0, &record), CalculationRecordStatus::Ok);
        const CalculationEngine::RestoreOutcome outcome = engine("a").restoreResult(record.result);
        QCOMPARE(outcome.kind, CalculationEngine::RestoreOutcome::Kind::Stale);
        QCOMPARE(outcome.staleCheck, CalculationEngine::RestoreOutcome::StaleCheck::Fingerprint);
        QVERIFY(isNotRequested(engine("a").resultStatus(kFit)));
    }
    QVERIFY(quiet.holds());
}

// ---- Validity follows the code -----------------------------------------------------------

void FusionStoreTest::codeStampChangeDropsRecordOnLoad_data()
{
    QTest::addColumn<QString>("stamp");
    for (const char *stamp : {"compatibility", "resultVersion", "environment"})
        QTest::newRow(stamp) << QString::fromLatin1(stamp);
}

// Spec 8: bumping the compatibility marker, the environment fingerprint or
// the calculation's result version drops the record on load.
void FusionStoreTest::codeStampChangeDropsRecordOnLoad()
{
    QFETCH(QString, stamp);
    const auto restoreCapacity = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    });
    bool extraRegistered = false;
    const auto unregisterExtra = qScopeGuard([&extraRegistered] {
        if (extraRegistered)
            CalculationRegistry::instance().unregister(kExtra);
    });

    QCOMPARE(addSessions({fixtureSession(QStringLiteral("coarse_linear"), QStringLiteral("a"))}), QString());
    show({"a"});
    check(QStringLiteral("roll"));
    QCOMPARE(m_queue->request("a", kFit).kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));
    QVERIFY(waitForIdle(*m_model));
    const QString path = recordPath("a");
    QVERIFY(QFileInfo(path).isFile());
    show({"a"}, false);
    session("a");
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 0);
    QVERIFY(!isLoaded("a"));
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);

    if (stamp == QLatin1String("compatibility")) {
        QCOMPARE(rewriteRecord("a", [](CalculationRecord &r) { r.calculationCompatibility += 1; }), QString());
    } else if (stamp == QLatin1String("resultVersion")) {
        QCOMPARE(rewriteRecord("a", [](CalculationRecord &r) {
                     r.result.resultVersion = QStringLiteral("batch-temperature-bias-v2");
                 }), QString());
    } else if (stamp == QLatin1String("environment")) {
        CalculationDescriptor extra;
        extra.id = kExtra;
        extra.outputs = {DependencyKey::attribute(QStringLiteral("_STORE_EXTRA"))};
        extra.compute = [](const EvaluationContext &) {
            return CalculationResult().setAttribute(QStringLiteral("_STORE_EXTRA"), 1);
        };
        extraRegistered = CalculationRegistry::instance().registerCalculation(extra);
        QVERIFY(extraRegistered);
    } else {
        QFAIL("unknown stamp");
    }
    QVERIFY(QFileInfo(path).isFile());
    m_model->resetStoredResultStats();

    const int jobs = m_queue->model()->rowCount();
    const Quiet quiet(*m_queue);
    show({"a"});
    QVERIFY(isLoaded("a"));

    QVERIFY(!QFileInfo::exists(path));
    QCOMPARE(stats().staleRecordsDeleted, 1);
    QVERIFY(engine("a").resultStatus(kFit) != std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(engine("a").readiness(kFit).state, CalculationReadiness::State::Ready);
    QVERIFY2(availableIn("a").isEmpty(), qPrintable(availableIn("a")));
    QCOMPARE(engine("a").runCount(kFit), 0);
    const PlotRowState state = row(kRoll);
    QCOMPARE(state.missingCount, 1);
    QCOMPARE(state.control(), Control::Refresh);
    QCOMPARE(m_queue->model()->rowCount(), jobs);
    QVERIFY(quiet.holds());
}

// ---- The session file ------------------------------------------------------------------

// Spec 8: saving a session with a stored result gives the same bytes as
// without. The fit here is synchronous: the second install path.
void FusionStoreTest::sessionFileBytesUnaffectedByRecord()
{
    QCOMPARE(addSessions({fixtureSession(QStringLiteral("coarse_linear"), QStringLiteral("a"))}), QString());
    const QString csv = sessionFilePath("a");
    const QByteArray b0 = bytesOf(csv);
    QVERIFY(!b0.isEmpty());

    QCOMPARE(engine("a").request(kFit).status, ResultStatus::Ok);
    const QString path = recordPath("a");
    const QByteArray record = bytesOf(path);
    QVERIFY(!record.isEmpty());

    QVERIFY(LogbookManager::instance().saveSession(session("a")));
    QCOMPARE(bytesOf(csv), b0);
    QCOMPARE(bytesOf(path), record);
    QCOMPARE(sessionCsvFiles().size(), 1);
    for (const QByteArray &line : b0.split('\n'))
        QVERIFY2(!line.startsWith("$COL,Fusion"), line.constData());
}

// ---- Before the first save ---------------------------------------------------------------

void FusionStoreTest::fittedBeforeFirstSaveIsRestored_data()
{
    QTest::addColumn<bool>("viaRestart");
    QTest::newRow("evict") << false;
    QTest::newRow("restart") << true;
}

// (Coordinator) a session imported and fitted before the idle saver ran: the
// record is written under the reserved stem, joined by the session file at
// the first save, and restored after eviction or a restart.
void FusionStoreTest::fittedBeforeFirstSaveIsRestored()
{
    QFETCH(bool, viaRestart);
    const auto restoreCapacity = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    });

    // Not addSessions(): that waits for the saver
    const QList<MergeResult> results =
        m_model->mergeSessions(QList<SessionData>{fixtureSession(QStringLiteral("coarse_linear"), QStringLiteral("a"))});
    QCOMPARE(results.size(), 1);
    QCOMPARE(results.at(0).outcome, MergeResult::Outcome::Created);
    // Same event-loop pass: the saver cannot run during a synchronous call
    QCOMPARE(engine("a").request(kFit).status, ResultStatus::Ok);

    QCOMPARE(sessionCsvFiles(), QStringList());
    const QStringList records = calculationRecordFiles();
    QCOMPARE(records.size(), 1);
    QVERIFY2(records.at(0).endsWith(kRecordSuffix), qPrintable(records.at(0)));
    QCOMPARE(stats().recordsWritten, 1);

    const FitValues fresh = capture("a");
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(!sessionFileStem("a").isEmpty());
    QCOMPARE(records.at(0), sessionFileStem("a") + kRecordSuffix);
    QCOMPARE(sessionCsvFiles().size(), 1);

    if (viaRestart) {
        restart();
        const Quiet quiet(*m_queue);
        show({"a"});
        QVERIFY(isLoaded("a"));
        QVERIFY(quiet.holds());
    } else {
        m_model->resetStoredResultStats();
        const Quiet quiet(*m_queue);
        QCOMPARE(unloadAndReload("a"), QString());
        QVERIFY(quiet.holds());
    }

    QString difference = differenceFrom(fresh, "a");
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    difference = goldenDifference(session("a"), loadFusionGolden(QStringLiteral("coarse_linear")));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    QCOMPARE(engine("a").runCount(kFit), 0);
    QCOMPARE(engine("a").readiness(kFit).state, CalculationReadiness::State::Done);
    QCOMPARE(m_queue->model()->rowCount(), 0);
    QCOMPARE(stats().recordsRestored, 1);
}

FLYSIGHT_TEST_MAIN(FusionStoreTest)
#include "tst_fusion_store.moc"
