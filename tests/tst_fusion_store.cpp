// Stored fusion results end to end (store-requested-calculations, spec section
// 8): SessionModel + the executor + PlotModel +
// CalculationDemand + Fusion::registerFusionCalculations on a real temporary
// logbook, with real fits. Where the roll plot is checked for a visible
// session, the demand layer starts the fit; restoring is not requesting.
//
//  - a fit survives unload and restart bit for bit (the seventeen channels,
//    the derived values, the diagnostics and the detail equal the fresh
//    publish, and the goldens), with no job, no run and a plain row;
//  - a rejection and a solver failure come back with their reason and badge;
//  - validity follows the inputs (an unrelated edit keeps the record, a
//    dependency edit or an IMU merge drops it), what the fit looked up and the
//    code stamps; nothing unrelated to what it reached (altitude markers,
//    other registrations, the descent-pause preference, another plug-in set)
//    makes it stale, in memory or across a restart; a registry change that
//    changes what a name it looked up resolves to drops it and its record at
//    once, a candidate registered behind the provider does not; a lookup
//    that resolves differently at load deletes it;
//  - a fit published before the session's first save is stored and restored;
//  - the session file's bytes never depend on a record;
//  - the logbook's cache/ folder deleted while the application is closed: the
//    fit reads not requested at the next start, and nothing runs until the
//    demand layer offers it;
//  - a logbook column over a fusion output ("roll @ exit") is filled for
//    sessions that are not loaded, stored, and not fitted again at the next
//    start.
//
// The oracle (verifyAgainstFresh / evaluateFresh) is never used on a session
// with the fit installed: it would run the fit again. Expected values are
// literals and the committed goldens of the kernel.

#include <functional>
#include <memory>
#include <optional>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QtTest>

#include "altitudemarkerfeature.h"
#include "calculationdemand.h"
#include "calculationrecord.h"
#include "calculations/builtincalculations.h"
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

using Kind = JobQueue::OfferResult::Kind;

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

/// The entry of `resolutions` for `name`; nullptr when there is none.
const StoredResolution *resolutionOf(const QList<StoredResolution> &resolutions, const DependencyKey &name)
{
    for (const StoredResolution &r : resolutions) {
        if (r.name == name)
            return &r;
    }
    return nullptr;
}

/// An OnDemand calculation with no inputs and one attribute output.
CalculationDescriptor constantAttribute(const QString &id, const QString &output, const QVariant &value)
{
    CalculationDescriptor d;
    d.id = id;
    d.outputs = {DependencyKey::attribute(output)};
    d.compute = [output, value](const EvaluationContext &) {
        return CalculationResult().setAttribute(output, value);
    };
    return d;
}

/// A stand-in plug-in calculation: constantAttribute() declaring as its
/// result version the plug-in code identity of a folder whose a_plugin.py
/// holds `aPlugin`.
CalculationDescriptor pluginStandIn(const QString &id, const QString &output, double value,
                                    const QByteArray &aPlugin)
{
    CalculationDescriptor d = constantAttribute(id, output, value);
    d.resultVersion = standInPluginIdentity(aPlugin);
    return d;
}

/// OnDemand: GNSS/sAcc = the samples of GNSS/testSAcc, unit "m/s".
CalculationDescriptor sAccFrom(const QString &id)
{
    CalculationDescriptor d;
    d.id = id;
    d.inputs = {CalcInput::measurement(QStringLiteral("GNSS"), QStringLiteral("testSAcc"))};
    d.outputs = {DependencyKey::measurement(QStringLiteral("GNSS"), QStringLiteral("sAcc"))};
    d.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setMeasurement(QStringLiteral("GNSS"), QStringLiteral("sAcc"),
                                                  ctx.measurement(QStringLiteral("GNSS"), QStringLiteral("testSAcc")),
                                                  QStringLiteral("m/s"));
    };
    return d;
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
    void storedFitSurvivesUnrelatedChanges_data();
    void storedFitSurvivesUnrelatedChanges();
    void runtimeRegistryChangeDropsFitAndRecord();
    void lookupResolvingDifferentlyAtLoadDeletesFit();
    void sessionFileBytesUnaffectedByRecord();
    void fittedBeforeFirstSaveIsRestored_data();
    void fittedBeforeFirstSaveIsRestored();
    void deletedCacheFolderReadsNotRequested();
    void columnOverFusionFillsUnloadedSessions();
    void fusionColumnWithStoredFitsRunsNothing();

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
    /// A programmatic check: the same demand as a click, the Plots menu or a
    /// profile.
    void check(const QString &measurement, bool enabled = true)
    {
        m_plots->setPlotEnabled(QStringLiteral("Fusion"), measurement, enabled);
    }
    /// The current plot state: a pending pass runs first.
    DemandState row(const QString &plotId)
    {
        m_demand->flush();
        return m_demand->plotState(plotId);
    }
    const CalculationResultStore::Stats &stats() const { return m_model->storedResultStats(); }

    /// cache/<stem>.builtin%2Efusion%2Efit.fvresult; the stem comes from
    /// index.json on disk.
    static QString recordPath(const QString &id)
    {
        return TestEnvironment::instance().cacheDir() + QLatin1Char('/') + sessionFileStem(id) + kRecordSuffix;
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
    /// from the index, a new executor, plot model (unchecked) and demand layer.
    /// `whileClosed` runs after the old objects are gone and before initialize().
    void restart(const std::function<void()> &whileClosed = {});
    /// Reads the record, applies `mutate`, writes it back. Empty on success.
    [[nodiscard]] QString rewriteRecord(const QString &id, const std::function<void(CalculationRecord &)> &mutate);
    void watchLoad(QObject *scope, const QString &id, LoadWatch *out);
    /// storedFitSurvivesUnrelatedChanges' runtime step on the loaded session
    /// "a": `apply` changes the calculation environment of the `concerned`
    /// names but not the one of the fit's outputs, and the fit stays
    /// installed, bit for bit, with its record. Empty when all of that held.
    [[nodiscard]] QString runtimeStep(const std::function<void()> &apply, const QList<DependencyKey> &concerned,
                                      const QByteArray &r0, const FitValues &fresh);
    /// Its restart check: after restart(whileClosed) the fit of "a" is
    /// restored when the row is shown, with no job. Empty when that held.
    [[nodiscard]] QString restartCheck(const std::function<void()> &whileClosed, const QByteArray &r0,
                                       const FitValues &fresh);
    /// Empty when the blocker reports agree in state, blockers and notes.
    static QString reportDifference(const BlockerReport &got, const BlockerReport &expected);
    /// For a visible track whose fit has just become missing while roll is
    /// checked: the roll row waits on it, and exactly one job has been offered
    /// since `queued` (a jobQueued spy) was made, when the executor held
    /// `jobsBefore` records - the fit of `id`, still Queued. Unchecking roll
    /// ends it Cancelled ("No longer needed") at once, before it ever started,
    /// and nothing else is offered. Empty when all of that held.
    [[nodiscard]] QString offeredFitIsDroppedByUncheck(const QString &id, const QSignalSpy &queued, int jobsBefore);

    std::unique_ptr<SessionModel> m_model;
    std::unique_ptr<JobQueue> m_queue;
    std::unique_ptr<PlotModel> m_plots;
    std::unique_ptr<CalculationDemand> m_demand;
    std::unique_ptr<ExtraRegistrations> m_extra;
    std::unique_ptr<AltitudeMarkerManager> m_altitudes;
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
    m_extra = std::make_unique<ExtraRegistrations>();

    m_model = std::make_unique<SessionModel>();
    m_queue = std::make_unique<JobQueue>(m_model.get());
    m_plots = std::make_unique<PlotModel>();
    m_plots->setPlots(fusionPlots());
    m_demand = std::make_unique<CalculationDemand>(m_model.get(), m_plots.get(), m_queue.get());
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

    // Reverse order of construction: the demand layer before the executor
    m_demand.reset();
    m_plots.reset();
    m_queue.reset();
    m_model.reset();
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    // After the model: registrations removed with a live model would reach it
    m_altitudes.reset();
    writeAltitudes({});
    m_extra.reset();

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

void FusionStoreTest::restart(const std::function<void()> &whileClosed)
{
    if (m_queue)
        m_queue->shutdown();
    m_demand.reset();
    m_plots.reset();
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
    m_plots = std::make_unique<PlotModel>();
    m_plots->setPlots(fusionPlots());
    m_demand = std::make_unique<CalculationDemand>(m_model.get(), m_plots.get(), m_queue.get());
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

QString FusionStoreTest::runtimeStep(const std::function<void()> &apply, const QList<DependencyKey> &concerned,
                                     const QByteArray &r0, const FitValues &fresh)
{
    const QList<DependencyKey> fitOutputs = {DependencyKey::measurement(QStringLiteral("Fusion"), QStringLiteral("roll"))};
    const QString env0 = calculationEnvironmentDigest(concerned);
    const QString fitEnv0 = calculationEnvironmentDigest(fitOutputs);
    const int runs = engine("a").runCount(kFit);
    m_model->resetStoredResultStats();
    const Quiet quiet(*m_queue);
    apply();
    m_model->flushPendingInvalidations();
    if (calculationEnvironmentDigest(concerned) == env0)
        return QStringLiteral("the environment of the concerned names did not change");
    if (calculationEnvironmentDigest(fitOutputs) != fitEnv0)
        return QStringLiteral("the environment of the fit's outputs changed");
    if (engine("a").resultStatus(kFit) != std::optional<ResultStatus>(ResultStatus::Ok))
        return QStringLiteral("the fit was dropped");
    if (engine("a").runCount(kFit) != runs)
        return QStringLiteral("the fit ran");
    if (bytesOf(recordPath("a")) != r0)
        return QStringLiteral("the record changed");
    if (stats().droppedRecordsDeleted != 0)
        return QStringLiteral("a record was deleted");
    const QString difference = differenceFrom(fresh, "a");
    if (!difference.isEmpty())
        return difference;
    // Whatever the demand layer was going to start by itself has started
    PlotFixture::spin(m_demand.get());
    if (!quiet.holds())
        return QStringLiteral("a job started");
    return QString();
}

QString FusionStoreTest::restartCheck(const std::function<void()> &whileClosed, const QByteArray &r0,
                                      const FitValues &fresh)
{
    restart(whileClosed);
    check(QStringLiteral("roll"));
    const Quiet quiet(*m_queue);
    show({"a"});
    if (!isLoaded("a"))
        return QStringLiteral("a was not loaded");
    if (engine("a").resultStatus(kFit) != std::optional<ResultStatus>(ResultStatus::Ok))
        return QStringLiteral("the fit was not restored");
    if (engine("a").runCount(kFit) != 0)
        return QStringLiteral("the fit ran");
    if (stats().recordsRestored != 1 || stats().staleRecordsDeleted != 0)
        return QStringLiteral("%1 restored, %2 deleted").arg(stats().recordsRestored).arg(stats().staleRecordsDeleted);
    if (bytesOf(recordPath("a")) != r0)
        return QStringLiteral("the record changed");
    const QString difference = differenceFrom(fresh, "a");
    if (!difference.isEmpty())
        return difference;
    // Restoring is not requesting: the demand layer, too, finds nothing to start
    PlotFixture::spin(m_demand.get());
    if (m_queue->model()->rowCount() != 0)
        return QStringLiteral("a job was created");
    if (!quiet.holds())
        return QStringLiteral("a job started");
    return QString();
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

QString FusionStoreTest::offeredFitIsDroppedByUncheck(const QString &id, const QSignalSpy &queued, int jobsBefore)
{
    const DemandState state = row(kRoll);
    if (state.waitingCount != 1 || state.runningCount != 0)
        return QStringLiteral("the row waits on %1 and runs %2 tracks").arg(state.waitingCount).arg(state.runningCount);
    if (queued.count() != 1 || m_queue->model()->rowCount() != jobsBefore + 1)
        return QStringLiteral("%1 jobs were offered, %2 created")
            .arg(queued.count()).arg(m_queue->model()->rowCount() - jobsBefore);
    const JobRecord offered = m_queue->model()->record(jobsBefore);
    if (offered.sessionId != id || offered.calculationId != kFit || offered.state != JobState::Queued)
        return QStringLiteral("the offered job is not the queued fit of ") + id;
    if (m_queue->chosenNextJob() != offered.id || state.waiting.at(0).job != offered.id)
        return QStringLiteral("the offered job is not the chosen next job the row waits on");

    check(QStringLiteral("roll"), false);
    const JobRecord dropped = m_queue->job(offered.id);     // at once, before any event-loop turn
    if (dropped.state != JobState::Cancelled || dropped.reason != QStringLiteral("No longer needed"))
        return QStringLiteral("unchecking left the job %1 (%2)").arg(int(dropped.state)).arg(dropped.reason);
    if (dropped.startedAt.isValid())
        return QStringLiteral("the dropped job had started");

    PlotFixture::spin(m_demand.get());
    if (queued.count() != 1 || m_queue->model()->rowCount() != jobsBefore + 1)
        return QStringLiteral("another job was offered");
    if (!m_queue->isIdle())
        return QStringLiteral("the executor is not idle");
    if (engine(id).runCount(kFit) != 0)
        return QStringLiteral("the fit ran");
    if (row(kRoll) != DemandState())
        return QStringLiteral("the unchecked row is not plain");
    return QString();
}

// ---- Bit-identical restores ------------------------------------------------------------

// Spec 8: fitted, saved, unloaded, reloaded: the goldens, no job, a plain row.
void FusionStoreTest::restoredAfterEvictionIsBitIdentical()
{
    const auto restoreCapacity = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    });
    QCOMPARE(addSessions({fixtureSession(QStringLiteral("coarse_maneuver"), QStringLiteral("a"))}), QString());
    show({"a"});
    check(QStringLiteral("roll"));      // the demand layer starts the fit
    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));
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
    QCOMPARE(m_queue->offer("a", kFit).kind, Kind::NothingToDo);
    QCOMPARE(m_queue->model()->rowCount(), 1);      // only the first job
    QVERIFY(quiet.holds());

    const DemandState state = row(kRoll);
    QVERIFY(state.isPlain());
    PlotFixture::spin(m_demand.get());
    QVERIFY(quiet.holds());
    QCOMPARE(m_queue->model()->rowCount(), 1);
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
    QCOMPARE(m_queue->offer("a", kFit).kind, Kind::Created);
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
    QCOMPARE(m_queue->offer("a", kFit).kind, Kind::NothingToDo);
    QCOMPARE(m_queue->model()->rowCount(), 0);
    QVERIFY(quiet.holds());

    const DemandState state = row(kRoll);
    QVERIFY(state.isPlain());
    QCOMPARE(state.doneCount, 1);
    PlotFixture::spin(m_demand.get());
    QCOMPARE(m_queue->model()->rowCount(), 0);
    QVERIFY(quiet.holds());
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
// badge; no job runs.
void FusionStoreTest::restoredRejectionShowsBadge()
{
    const auto restoreCapacity = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    });
    QCOMPARE(addSessions({sessionFromFixture(fusionFixture(QStringLiteral("reject_origin")), QStringLiteral("r1"))}),
             QString());
    show({"r1"});
    check(QStringLiteral("roll"));      // the demand layer starts the fit
    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(m_queue->model()->record(0).state, JobState::Succeeded);    // a rejection is a result
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(QFileInfo(recordPath("r1")).isFile());
    const BlockerReport freshBlockers = engine("r1").blockers(fusionKey(QStringLiteral("roll")));
    const QString freshDiagnostics = session("r1").getAttribute(kDiagnostics).toString();
    QVERIFY(!freshDiagnostics.isEmpty());

    const Quiet quiet(*m_queue);
    QCOMPARE(unloadAndReload("r1"), QString());

    const DemandState state = row(kRoll);
    QCOMPARE(state.failedCount, 1);
    QVERIFY(state.showsWarning());
    QVERIFY(!state.isWorking());
    QVERIFY(!state.failed.at(0).jobFailure);
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

    // No rerun: the same inputs give the same answer
    for (int i = 0; i < 3; ++i)
        PlotFixture::spin(m_demand.get());
    QVERIFY(quiet.holds());
    QVERIFY(row(kRoll).showsWarning());
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
    check(QStringLiteral("roll"));      // the demand layer starts the fit
    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));
    QCOMPARE(m_queue->model()->rowCount(), 1);
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

    const DemandState state = row(kRoll);
    QCOMPARE(state.failedCount, 1);
    QVERIFY(state.showsWarning());
    QVERIFY(!state.failed.at(0).jobFailure);
    QCOMPARE(state.failed.at(0).reason, QStringLiteral("Sensor fusion: ") + kSolverFailureReason);

    const BlockerReport blockers = engine("r1").blockers(fusionKey(QStringLiteral("roll")));
    QCOMPARE(blockers.state, BlockerReport::State::NotProduced);
    QCOMPARE(blockers.notProduced.size(), 1);
    QCOMPARE(blockers.notProduced.at(0).detail, kSolverFailureReason);

    QCOMPARE(session("r1").getAttribute(kDiagnostics).toString().toUtf8(), kSolverFailureDiagnostics.toUtf8());
    // No rerun: a stored failure is a result
    for (int i = 0; i < 3; ++i)
        PlotFixture::spin(m_demand.get());
    QVERIFY(row(kRoll).showsWarning());
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
    QCOMPARE(m_queue->offer("a", kFit).kind, Kind::Created);
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
// the calculation reads not requested, and the row waits for the inputs to
// settle (nothing starts during the wait).
void FusionStoreTest::dependencyEditDropsRecord()
{
    QFETCH(QString, key);
    QFETCH(QVariant, value);
    const auto restoreCapacity = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    });
    QCOMPARE(addSessions({fixtureSession(QStringLiteral("coarse_maneuver"), QStringLiteral("a"))}), QString());
    show({"a"});
    check(QStringLiteral("roll"));      // the demand layer starts the fit
    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QVERIFY(waitForIdle(*m_model));
    const QString path = recordPath("a");
    QVERIFY(QFileInfo(path).isFile());
    m_model->resetStoredResultStats();

    m_demand->setInputSettleDelay(60000);
    const Quiet quiet(*m_queue);
    QVERIFY(session("a").getAttribute(key) != value);
    QVERIFY(m_model->updateAttribute("a", key, value));
    // No event-loop pass in between
    QVERIFY(!QFileInfo::exists(path));
    QCOMPARE(stats().droppedRecordsDeleted, 1);
    QVERIFY(isNotRequested(engine("a").resultStatus(kFit)));
    QVERIFY2(availableIn("a").isEmpty(), qPrintable(availableIn("a")));

    const DemandState state = row(kRoll);
    QCOMPARE(state.waitingCount, 1);
    QCOMPARE(state.runningCount, 0);
    QVERIFY(state.waiting.at(0).settling);
    QCOMPARE(state.waiting.at(0).job, JobId(0));
    QVERIFY(m_demand->isSettling(QStringLiteral("a")));
    PlotFixture::spin(m_demand.get());
    QVERIFY(quiet.holds());

    // Unchecked: nothing to drop (nothing was offered)
    check(QStringLiteral("roll"), false);
    QVERIFY(row(kRoll) == DemandState());
    QVERIFY(quiet.holds());

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
    QCOMPARE(m_queue->offer("a", kFit).kind, Kind::Created);
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
    QCOMPARE(m_queue->offer("a", kFit).kind, Kind::Created);
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
    for (const char *stamp : {"compatibility", "resultVersion", "providerResultVersion"})
        QTest::newRow(stamp) << QString::fromLatin1(stamp);
}

// Bumping the compatibility marker, the fit's result version, or the result
// version recorded for a calculation its lookups went through drops the
// record on load.
void FusionStoreTest::codeStampChangeDropsRecordOnLoad()
{
    QFETCH(QString, stamp);
    const auto restoreCapacity = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    });

    QCOMPARE(addSessions({fixtureSession(QStringLiteral("coarse_linear"), QStringLiteral("a"))}), QString());
    show({"a"});
    check(QStringLiteral("roll"));      // the demand layer starts the fit
    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));
    QCOMPARE(m_queue->model()->rowCount(), 1);
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
    } else if (stamp == QLatin1String("providerResultVersion")) {
        bool found = false;
        bool byCalculation = false;
        QCOMPARE(rewriteRecord("a", [&](CalculationRecord &r) {
                     for (StoredResolution &resolution : r.result.resolutions) {
                         if (resolution.name == DependencyKey::measurement(QStringLiteral("IMU"), QStringLiteral("az"))) {
                             found = true;
                             byCalculation = resolution.provider == StoredResolution::Provider::Calculation;
                             resolution.resultVersion = QStringLiteral("v-old");
                         }
                     }
                 }), QString());
        QVERIFY(found);
        QVERIFY(byCalculation);
    } else {
        QFAIL("unknown stamp");
    }
    QVERIFY(QFileInfo(path).isFile());
    m_model->resetStoredResultStats();

    const int jobs = m_queue->model()->rowCount();
    const QSignalSpy queued(m_queue.get(), &JobQueue::jobQueued);
    show({"a"});
    QVERIFY(isLoaded("a"));

    QVERIFY(!QFileInfo::exists(path));
    QCOMPARE(stats().staleRecordsDeleted, 1);
    QVERIFY(engine("a").resultStatus(kFit) != std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(engine("a").readiness(kFit).state, CalculationReadiness::State::Ready);
    QVERIFY2(availableIn("a").isEmpty(), qPrintable(availableIn("a")));
    QCOMPARE(engine("a").runCount(kFit), 0);
    // The stale record is gone, so the checked plot wants the fit again
    QCOMPARE(offeredFitIsDroppedByUncheck("a", queued, jobs), QString());
}

void FusionStoreTest::storedFitSurvivesUnrelatedChanges_data()
{
    QTest::addColumn<QString>("change");
    for (const char *change : {"altitudeMarker", "unrelatedCalculation", "descentPause", "unrelatedPluginSet"})
        QTest::newRow(change) << QString::fromLatin1(change);
}

// Spec 10, first item: changes of the calculation environment that the fit
// never reached (an altitude marker, a calculation of a name it never looks
// up, the descent-pause preference, another plug-in set) leave the installed
// fit and its record alone, and the record is restored with no job after a
// restart with the change in effect.
void FusionStoreTest::storedFitSurvivesUnrelatedChanges()
{
    QFETCH(QString, change);
    const auto restoreCapacity = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    });
    QCOMPARE(addSessions({fixtureSession(QStringLiteral("coarse_linear"), QStringLiteral("a"))}), QString());
    show({"a"});
    check(QStringLiteral("roll"));      // the demand layer starts the fit
    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QVERIFY(waitForIdle(*m_model));
    const QByteArray r0 = bytesOf(recordPath("a"));
    QVERIFY(!r0.isEmpty());
    const FitValues fresh = capture("a");
    PreferencesManager &prefs = PreferencesManager::instance();

    if (change == QLatin1String("altitudeMarker")) {
        m_altitudes = std::make_unique<AltitudeMarkerManager>();
        const QList<DependencyKey> marker = {DependencyKey::attribute(QStringLiteral("_ALTITUDE_1000_FT"))};
        QCOMPARE(runtimeStep([] { writeAltitudes({1000}); }, marker, r0, fresh), QString());
        QVERIFY(CalculationRegistry::instance().contains(QStringLiteral("builtin.altitude._ALTITUDE_1000_FT")));
        m_model->resetStoredResultStats();
        QCOMPARE(unloadAndReload("a"), QString());
        QCOMPARE(stats().recordsRestored, 1);
        QCOMPARE(engine("a").runCount(kFit), 0);
        // The manager lives on: the next run registers the same marker
        QCOMPARE(restartCheck({}, r0, fresh), QString());
        QCOMPARE(runtimeStep([] { writeAltitudes({}); }, marker, r0, fresh), QString());
        QVERIFY(!CalculationRegistry::instance().contains(QStringLiteral("builtin.altitude._ALTITUDE_1000_FT")));
        QCOMPARE(restartCheck({}, r0, fresh), QString());
    } else if (change == QLatin1String("unrelatedCalculation")) {
        const CalculationDescriptor extra = constantAttribute(kExtra, QStringLiteral("_STORE_EXTRA"), 1);
        const QList<DependencyKey> name = {DependencyKey::attribute(QStringLiteral("_STORE_EXTRA"))};
        bool changed = false;
        QCOMPARE(runtimeStep([&] { changed = m_extra->add(extra); }, name, r0, fresh), QString());
        QVERIFY(changed);
        QCOMPARE(restartCheck({}, r0, fresh), QString());
        QCOMPARE(runtimeStep([&] { changed = m_extra->remove(kExtra); }, name, r0, fresh), QString());
        QVERIFY(changed);
        QCOMPARE(restartCheck({}, r0, fresh), QString());
    } else if (change == QLatin1String("descentPause")) {
        CalculationRecord record;
        QCOMPARE(decodeCalculationRecord(r0, &record), CalculationRecordStatus::Ok);
        QVERIFY(!record.result.leaves.contains(GraphNode::preference(PreferenceKeys::ImportDescentPauseSeconds)));
        const QList<DependencyKey> exitName = {DependencyKey::attribute(QStringLiteral("_EXIT_TIME"))};
        QCOMPARE(runtimeStep([&] { prefs.setValue(PreferenceKeys::ImportDescentPauseSeconds, 45.0); }, exitName, r0,
                             fresh),
                 QString());
        QCOMPARE(restartCheck({}, r0, fresh), QString());
        QCOMPARE(runtimeStep([&] { prefs.setValue(PreferenceKeys::ImportDescentPauseSeconds, 30.0); }, exitName, r0,
                             fresh),
                 QString());
        QCOMPARE(restartCheck({}, r0, fresh), QString());
    } else if (change == QLatin1String("unrelatedPluginSet")) {
        const QString a = QStringLiteral("test.plugin.a");
        const QString b = QStringLiteral("test.plugin.b");
        const CalculationDescriptor setA =
            pluginStandIn(a, QStringLiteral("_TEST_PLUGIN_A"), 1.0, QByteArray("a = 1\n"));
        const CalculationDescriptor setB =
            pluginStandIn(b, QStringLiteral("_TEST_PLUGIN_B"), 2.0, QByteArray("b = 1\n"));
        QVERIFY(setA.resultVersion != setB.resultVersion);
        const QList<std::function<bool()>> runs = {
            [&] { return m_extra->add(setA); },
            [&] { return m_extra->remove(a) && m_extra->add(setB); },
            [&] { return m_extra->remove(b); }};
        const QList<DependencyKey> pluginNames = {DependencyKey::attribute(QStringLiteral("_TEST_PLUGIN_A")),
                                                  DependencyKey::attribute(QStringLiteral("_TEST_PLUGIN_B"))};
        const QList<DependencyKey> fitOutputs = {
            DependencyKey::measurement(QStringLiteral("Fusion"), QStringLiteral("roll"))};
        for (const std::function<bool()> &nextRun : runs) {
            const QString env0 = calculationEnvironmentDigest(pluginNames);
            const QString fitEnv0 = calculationEnvironmentDigest(fitOutputs);
            bool loaded = false;
            QCOMPARE(restartCheck([&] { loaded = nextRun(); }, r0, fresh), QString());
            QVERIFY(loaded);
            QVERIFY(calculationEnvironmentDigest(pluginNames) != env0);
            QCOMPARE(calculationEnvironmentDigest(fitOutputs), fitEnv0);
        }
    } else {
        QFAIL("unknown change");
    }
}

// Spec 10, second item, as the engine settles it: a registry change made
// while the application runs drops the installed fit and deletes its record
// at once exactly when it changes what a name the fit looked up resolves to;
// the row then waits on the fit again. Here GNSS/sAcc, which only a
// registered calculation (sacc1) provides: a second candidate for it,
// registered behind sacc1, is never tried and changes nothing; removing sacc1
// hands the name to that candidate, which drops the fit and its record.
void FusionStoreTest::runtimeRegistryChangeDropsFitAndRecord()
{
    const auto restoreCapacity = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    });
    const QString sacc0 = QStringLiteral("test.store.sacc0");
    const QString sacc1 = QStringLiteral("test.store.sacc1");
    QVERIFY(m_extra->add(sAccFrom(sacc1)));
    QCOMPARE(addSessions({fixtureSessionWithSAccStoredAs(QStringLiteral("coarse_linear"), QStringLiteral("a"),
                                                         QStringLiteral("testSAcc"))}),
             QString());
    show({"a"});
    check(QStringLiteral("roll"));      // the demand layer starts the fit
    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(m_queue->model()->record(0).state, JobState::Succeeded);
    QVERIFY(waitForIdle(*m_model));
    const QString path = recordPath("a");
    const QByteArray r0 = bytesOf(path);
    QVERIFY(!r0.isEmpty());
    CalculationRecord record;
    QCOMPARE(decodeCalculationRecord(r0, &record), CalculationRecordStatus::Ok);
    const StoredResolution *sAcc = resolutionOf(record.result.resolutions,
                                                DependencyKey::measurement(QStringLiteral("GNSS"), QStringLiteral("sAcc")));
    QVERIFY(sAcc);
    QCOMPARE(sAcc->instanceId, sacc1);

    m_model->resetStoredResultStats();
    {
        const Quiet quiet(*m_queue);
        // A candidate behind the provider: nothing is dropped or deleted, and
        // the demand layer finds nothing to start
        QVERIFY(m_extra->add(sAccFrom(sacc0)));
        QCOMPARE(engine("a").resultStatus(kFit), std::optional<ResultStatus>(ResultStatus::Ok));
        QCOMPARE(bytesOf(path), r0);
        QCOMPARE(stats().droppedRecordsDeleted, 0);
        QVERIFY(row(kRoll).isPlain());
        PlotFixture::spin(m_demand.get());
        QVERIFY(quiet.holds());
    }

    // The provider removed: no event-loop pass in between
    const int jobs = m_queue->model()->rowCount();
    QVERIFY(m_extra->remove(sacc1));
    QVERIFY(!QFileInfo::exists(path));
    QCOMPARE(stats().droppedRecordsDeleted, 1);
    QVERIFY(isNotRequested(engine("a").resultStatus(kFit)));
    QVERIFY2(availableIn("a").isEmpty(), qPrintable(availableIn("a")));

    // The checked plot wants the fit again; unchecking drops whatever was
    // offered for it before it can start
    const DemandState state = row(kRoll);
    QCOMPARE(state.waitingCount, 1);
    QCOMPARE(state.runningCount, 0);
    QCOMPARE(state.waiting.at(0).sessionId, QStringLiteral("a"));
    check(QStringLiteral("roll"), false);
    PlotFixture::spin(m_demand.get());
    QVERIFY(m_queue->isIdle());
    QVERIFY(m_queue->model()->rowCount() <= jobs + 1);
    for (int r = jobs; r < m_queue->model()->rowCount(); ++r) {
        const JobRecord dropped = m_queue->model()->record(r);
        QCOMPARE(dropped.sessionId, QStringLiteral("a"));
        QCOMPARE(dropped.state, JobState::Cancelled);
        QCOMPARE(dropped.reason, QStringLiteral("No longer needed"));
        QVERIFY(!dropped.startedAt.isValid());
    }
    QCOMPARE(engine("a").runCount(kFit), 1);        // the first fit only

    QVERIFY(waitForIdle(*m_model));
    m_model->resetStoredResultStats();
    QCOMPARE(unloadAndReload("a"), QString());
    QCOMPARE(stats().recordsRead, 0);
    QVERIFY(isNotRequested(engine("a").resultStatus(kFit)));
    QCOMPARE(engine("a").runCount(kFit), 0);
}

// Spec 10, third item: a new candidate for a looked-up name (GNSS/sAcc, which
// only a registered calculation provides here), computing from the same
// inputs and tried first, registered before the load: the record is deleted
// and the fit reads not requested.
void FusionStoreTest::lookupResolvingDifferentlyAtLoadDeletesFit()
{
    const QString sacc0 = QStringLiteral("test.store.sacc0");
    const QString sacc1 = QStringLiteral("test.store.sacc1");
    QVERIFY(m_extra->add(sAccFrom(sacc1)));
    QCOMPARE(addSessions({fixtureSessionWithSAccStoredAs(QStringLiteral("coarse_linear"), QStringLiteral("a"),
                                                         QStringLiteral("testSAcc"))}),
             QString());
    QCOMPARE(m_queue->offer("a", kFit).kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));
    QCOMPARE(m_queue->model()->record(0).state, JobState::Succeeded);
    QCOMPARE(engine("a").resultStatus(kFit), std::optional<ResultStatus>(ResultStatus::Ok));
    QVERIFY(waitForIdle(*m_model));
    const QString path = recordPath("a");
    const QByteArray r0 = bytesOf(path);
    QVERIFY(!r0.isEmpty());
    CalculationRecord record;
    QCOMPARE(decodeCalculationRecord(r0, &record), CalculationRecordStatus::Ok);
    const StoredResolution *sAcc = resolutionOf(record.result.resolutions,
                                                DependencyKey::measurement(QStringLiteral("GNSS"), QStringLiteral("sAcc")));
    QVERIFY(sAcc);
    QCOMPARE(sAcc->provider, StoredResolution::Provider::Calculation);
    QCOMPARE(sAcc->instanceId, sacc1);
    QVERIFY(sAcc->resultVersion.isEmpty());

    // Re-registering goes to the end: sacc0 is tried first at the next load
    bool reordered = false;
    restart([&] { reordered = m_extra->add(sAccFrom(sacc0)) && m_extra->remove(sacc1) && m_extra->add(sAccFrom(sacc1)); });
    QVERIFY(reordered);
    check(QStringLiteral("roll"));
    const QSignalSpy queued(m_queue.get(), &JobQueue::jobQueued);
    show({"a"});
    QVERIFY(isLoaded("a"));

    QVERIFY(!QFileInfo::exists(path));
    QCOMPARE(stats().staleRecordsDeleted, 1);
    QVERIFY(isNotRequested(engine("a").resultStatus(kFit)));
    QCOMPARE(engine("a").runCount(kFit), 0);
    // The stale record is gone, so the checked plot wants the fit again
    QCOMPARE(offeredFitIsDroppedByUncheck("a", queued, 0), QString());

    // Which check failed
    const CalculationEngine::RestoreOutcome outcome = engine("a").restoreResult(record.result);
    QCOMPARE(outcome.kind, CalculationEngine::RestoreOutcome::Kind::Stale);
    QCOMPARE(outcome.staleCheck, CalculationEngine::RestoreOutcome::StaleCheck::Resolutions);
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

// ---- The cache folder ----------------------------------------------------------------------

// Spec 6: everything in cache/ is derived. Deleted while the application is
// closed, the fit reads not requested at the next start: the checked plot wants
// it again (and unchecked, nothing ever starts), nothing is read or written,
// and the recording is untouched.
void FusionStoreTest::deletedCacheFolderReadsNotRequested()
{
    TestEnvironment &env = TestEnvironment::instance();
    QCOMPARE(addSessions({fixtureSession(QStringLiteral("stationary_spin"), QStringLiteral("a"))}), QString());
    QCOMPARE(m_queue->offer("a", kFit).kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));
    QCOMPARE(m_queue->model()->record(0).state, JobState::Succeeded);
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(QFileInfo(recordPath("a")).isFile());
    const QString csvPath = sessionFilePath("a");
    const QByteArray csv = bytesOf(csvPath);
    QVERIFY(!csv.isEmpty());

    bool removed = false;
    restart([&] { removed = QDir(env.cacheDir()).removeRecursively(); });
    QVERIFY(removed);
    QVERIFY(LogbookManager::instance().knownCalculationRecords("a").isEmpty());

    check(QStringLiteral("roll"));
    const QSignalSpy queued(m_queue.get(), &JobQueue::jobQueued);
    show({"a"});
    QVERIFY(isLoaded("a"));

    QVERIFY(isNotRequested(engine("a").resultStatus(kFit)));
    QVERIFY2(availableIn("a").isEmpty(), qPrintable(availableIn("a")));
    QCOMPARE(engine("a").runCount(kFit), 0);
    QCOMPARE(engine("a").preparedCount(), 0);
    QCOMPARE(offeredFitIsDroppedByUncheck("a", queued, 0), QString());
    QCOMPARE(engine("a").preparedCount(), 0);

    QCOMPARE(stats().recordListings, 0);
    QCOMPARE(stats().recordsRead, 0);
    QCOMPARE(stats().recordsWritten, 0);
    QVERIFY(!QFileInfo::exists(env.cacheDir()));
    QCOMPARE(sessionFilePath("a"), csvPath);
    QCOMPARE(bytesOf(csvPath), csv);
}

// ---- A logbook column over a fusion output -----------------------------------------------

namespace {

/// Fusion/roll at the exit marker, the way the column editor makes it.
LogbookColumn rollAtExitColumn()
{
    LogbookColumn column;
    column.type = ColumnType::MeasurementAtMarker;
    column.sensorID = QStringLiteral("Fusion");
    column.measurementID = QStringLiteral("roll");
    for (const PlotValue &plot : fusionPlots()) {
        if (plot.measurementID == column.measurementID)
            column.measurementType = plot.measurementType;
    }
    column.markerAttributeKey = QString::fromLatin1(SessionKeys::ExitTime);
    return column;
}

} // namespace

// Enabling the column fills it for sessions that are not loaded: the session
// with IMU data is loaded as a hidden session, fitted, stored, and its value
// cached and indexed with the fit's stamp; the one without IMU data is not
// applicable and gets no job. Both leave as stubs by ordinary eviction.
void FusionStoreTest::columnOverFusionFillsUnloadedSessions()
{
    const auto restore = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
        LogbookColumnStore::instance().setColumns({descriptionColumn()});
    });
    const LogbookColumn roll = rollAtExitColumn();
    QVERIFY(!roll.measurementType.isEmpty());
    QCOMPARE(logbookColumnExplicitCalculations(roll, CalculationRegistry::instance()), QStringList({kFit}));

    QCOMPARE(addSessions({fixtureSession(QStringLiteral("coarse_maneuver"), QStringLiteral("a")),
                          sessionWithoutImu(fusionFixture(QStringLiteral("coarse_linear")), QStringLiteral("n1"))}),
             QString());
    QVERIFY(waitForIdle(*m_model));
    session("a");
    session("n1");                      // in the LRU list (a row never touched is not)
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 0);
    QVERIFY(!isLoaded("a"));
    QVERIFY(!isLoaded("n1"));

    LogbookColumnStore::instance().setColumns({descriptionColumn(), roll});
    const QString column = CalculationDemand::columnId(roll);
    m_demand->flush();
    QCOMPARE(m_demand->columnState(column).waitingCount, 2);
    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));
    QVERIFY(waitForIdle(*m_model));

    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(m_queue->model()->record(0).sessionId, QStringLiteral("a"));
    QCOMPARE(m_queue->model()->record(0).state, JobState::Succeeded);
    QVERIFY(QFileInfo(recordPath("a")).isFile());
    const DemandState state = m_demand->columnState(column);
    QVERIFY(state.isPlain());
    QCOMPARE(state.wantedCount, 1);
    QCOMPARE(state.doneCount, 1);
    QVERIFY(!isLoaded("a"));
    QVERIFY(!isLoaded("n1"));

    int section = -1;
    for (int c = 0; c < m_model->columnCount(); ++c) {
        if (CalculationDemand::columnId(m_model->column(c)) == column)
            section = c;
    }
    QVERIFY(section >= 0);
    const QVariant cachedRoll = std::as_const(*m_model).rowAt(m_model->getSessionRow("a")).cachedValues.value(section);
    QVERIFY(cachedRoll.isValid());
    const SessionRow &n1 = std::as_const(*m_model).rowAt(m_model->getSessionRow("n1"));
    QVERIFY(n1.cachedValues.contains(section));
    QVERIFY(!n1.cachedValues.value(section).isValid());
    m_model->flushDirtySessions();
    QCOMPARE(indexValue(QStringLiteral("a"), roll).toDouble(), cachedRoll.toDouble());
    QVERIFY(indexRecordStamp(QStringLiteral("a")).toObject().contains(kFit));

    // The value of a fresh load (compared only now: a load before the check
    // would have been a load of its own)
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    const QVariant loadedRoll = session("a").getAttribute(fusionRollAtExit());
    QVERIFY(loadedRoll.isValid());
    QVERIFY(sameBits(loadedRoll.toDouble(), cachedRoll.toDouble()));
    QCOMPARE(engine("a").runCount(kFit), 0);
}

// With every fit stored, the next start with the column enabled runs nothing
// and loads nothing: the record set says the column is done.
void FusionStoreTest::fusionColumnWithStoredFitsRunsNothing()
{
    const auto restore = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
        LogbookColumnStore::instance().setColumns({descriptionColumn()});
    });
    const LogbookColumn roll = rollAtExitColumn();
    const QString column = CalculationDemand::columnId(roll);

    QCOMPARE(addSessions({fixtureSession(QStringLiteral("coarse_maneuver"), QStringLiteral("a"))}), QString());
    QVERIFY(waitForIdle(*m_model));
    session("a");                       // in the LRU list (a row never touched is not)
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 0);
    QVERIFY(!isLoaded("a"));
    LogbookColumnStore::instance().setColumns({descriptionColumn(), roll});
    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(m_queue->model()->record(0).state, JobState::Succeeded);
    QVERIFY(QFileInfo(recordPath("a")).isFile());
    m_model->flushDirtySessions();
    const QJsonValue indexed = indexValue(QStringLiteral("a"), roll);
    QVERIFY(indexed.isDouble());

    restart();
    QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);
    {
        const Quiet quiet(*m_queue);
        m_demand->flush();
        const DemandState state = m_demand->columnState(column);
        QCOMPARE(state.doneCount, 1);
        QVERIFY(state.isPlain());
        PlotFixture::spin(m_demand.get());
        QVERIFY(waitForIdle(*m_model));
        PlotFixture::spin(m_demand.get());
        QVERIFY(quiet.holds());
    }
    QCOMPARE(loadedSpy.count(), 0);
    QCOMPARE(m_demand->columnState(column).doneCount, 1);
    QVERIFY(!m_demand->hasFillWork());
    int section = -1;
    for (int c = 0; c < m_model->columnCount(); ++c) {
        if (CalculationDemand::columnId(m_model->column(c)) == column)
            section = c;
    }
    QVERIFY(section >= 0);
    QCOMPARE(std::as_const(*m_model).rowAt(m_model->getSessionRow("a")).cachedValues.value(section).toDouble(),
             indexed.toDouble());
}

FLYSIGHT_TEST_MAIN(FusionStoreTest)
#include "tst_fusion_store.moc"
