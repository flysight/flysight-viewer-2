// The real fit through the job queue, on a real SessionModel, with the fit on
// the queue's 64 MiB worker. Sensor-fusion-jobs acceptance 5 (model level),
// 6, 7, 8 (first half), 9, 10 and 11; the rule that a logbook column over a
// fusion output is never cached; and the optional real-recording check.
//
// DETERMINISM WITHOUT A GATE. The real compute function cannot be held by a
// semaphore. The tests use the queue's ordering guarantee instead: progress
// posts reach the main thread in order and BEFORE the end of the same job is
// processed. A slot that acts on the first progress text of a job ("Starting
// fit") therefore runs while the queue still considers the job Running - even
// if the worker has returned meanwhile - and what it does (edit an input,
// cancel, shut down) is seen by the job's end. There are no sleeps.
//
// Expected values are the committed goldens and literals; the one computed
// comparison is "the queue gives what a synchronous request gives".

#include <functional>
#include <memory>

#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest>

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

using Kind = JobQueue::RequestResult::Kind;
using BlockerState = BlockerReport::State;
using ReadyState = CalculationReadiness::State;

Q_DECLARE_METATYPE(FlySight::DependencyKey)

namespace {

const QString kFit = QStringLiteral("builtin.fusion.fit");
const QString kDiagnostics = QStringLiteral("_FUSION_DIAGNOSTICS");

constexpr double kEpochUtc = 1700000000.0;
constexpr double kExitTime = kEpochUtc + 1.0;      // inside every success fixture's fit
constexpr int kRollColumn = 1;
constexpr int kFitTimeoutMs = 60000;

DependencyKey fusionKey(const QString &name)
{
    return DependencyKey::measurement(QStringLiteral("Fusion"), name);
}

/// Fusion/roll at the exit marker.
LogbookColumn rollColumn()
{
    LogbookColumn column;
    column.type = ColumnType::MeasurementAtMarker;
    column.sensorID = QStringLiteral("Fusion");
    column.measurementID = QStringLiteral("roll");
    column.measurementType = QStringLiteral("angle");
    column.markerAttributeKey = QString::fromLatin1(SessionKeys::ExitTime);
    return column;
}

SessionData fixtureSession(const QString &fixtureName, const QString &sessionId)
{
    SessionData session = sessionFromFixture(fusionFixture(fixtureName), sessionId);
    session.setAttribute(SessionKeys::ExitTime, kExitTime);
    return session;
}

bool sameBitsEverywhere(const QVector<double> &a, const QVector<double> &b)
{
    if (a.size() != b.size())
        return false;
    for (qsizetype i = 0; i < a.size(); ++i) {
        if (!sameBits(a[i], b[i]))
            return false;
    }
    return true;
}

} // namespace

class FusionJobsTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void jobPublishesAllOutputsTogether();
    void queueMatchesSynchronousRequest();
    void inputChangeDuringFitSupersedes();
    void rejectedRecordingIsSucceededJob();
    void cancelDuringFitThenNextJobStarts();
    void noImuSessionCannotHaveAJob();
    void readersNeverStartAFit();
    void columnOnFusionOutputIsNotCached();
    void shutdownDuringFit();
    void realRecordingCheck();

private:
    void addSessions(const QList<SessionData> &sessions);
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
    /// Empty when the published channels of the session match the golden.
    QString goldenDifference(const QString &id, const QString &goldenName);
    QJsonObject diagnosticsOf(const QString &id)
    {
        return QJsonDocument::fromJson(session(id).getAttribute(kDiagnostics).toString().toUtf8()).object();
    }
    /// Empty when nothing of the fit was published in the session; else what was found.
    QString publishedTrace(const QSignalSpy &dependencySpy, const QString &id);
    /// Runs `action` once, on the main thread, when the first progress text of
    /// `job` is delivered: the job is Running then (see the file comment).
    void onFirstProgress(JobId job, std::function<void()> action);

    std::unique_ptr<SessionModel> m_model;
    std::unique_ptr<JobQueue> m_queue;
    QStringList m_registryBefore;
};

void FusionJobsTest::initTestCase()
{
    // As the application does: every registration before the logbook and the
    // model exist (a live model reacts to registry changes).
    TestEnvironment::instance().registerBuiltIns();
    registerFusionOnce();

    PreferencesManager::instance().registerPreference(PreferenceKeys::LogbookColumnsVersion, 0);
    LogbookColumn description;
    description.type = ColumnType::SessionAttribute;
    description.attributeKey = QString::fromLatin1(SessionKeys::Description);
    LogbookColumnStore::instance().setColumns({description, rollColumn()});

    qRegisterMetaType<DependencyKey>();
}

void FusionJobsTest::init()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();
    env.resetPreferencesToDefaults();
    LogbookManager::instance().initialize();
    m_registryBefore = CalculationRegistry::instance().registeredIds();

    m_model = std::make_unique<SessionModel>();
    m_queue = std::make_unique<JobQueue>(m_model.get());
}

void FusionJobsTest::cleanup()
{
    if (m_queue)
        m_queue->shutdown();
    if (m_model) {
        for (int row = 0; row < m_model->rowCount(); ++row) {
            const QString id = std::as_const(*m_model).rowAt(row).sessionId;
            QVERIFY2(!m_model->isSessionPinned(id), qPrintable(id));
        }
    }

    // Queue, then model
    m_queue.reset();
    m_model.reset();
    QCOMPARE(CalculationRegistry::instance().registeredIds(), m_registryBefore);
    QCOMPARE(CalculationRegistry::instance().enrolledEngineCount(), 0);
}

// Sessions enter as they do in the application; the saver and the column
// worker have finished when this returns.
void FusionJobsTest::addSessions(const QList<SessionData> &sessions)
{
    m_model->mergeSessions(sessions);
    QCOMPARE(m_model->rowCount(), int(sessions.size()));
    QVERIFY(waitForIdle(*m_model));
    for (int row = 0; row < m_model->rowCount(); ++row)
        QVERIFY(std::as_const(*m_model).rowAt(row).isLoaded());
}

QString FusionJobsTest::availableIn(const QString &id)
{
    QStringList available;
    for (const DependencyKey &name : fusionNames()) {
        if (isAvailable(id, name))
            available.append(name.type == DependencyKey::Type::Attribute ? name.attributeKey
                                                                         : name.measurementKey.second);
    }
    return available.join(QStringLiteral(", "));
}

QString FusionJobsTest::goldenDifference(const QString &id, const QString &goldenName)
{
    const FusionGolden golden = loadFusionGolden(goldenName);
    for (const QString &name : fusionChannelNames()) {
        const QString difference = compareSamples(name, fusion(id, name), golden.channels.value(name));
        if (!difference.isEmpty())
            return difference;
    }
    return QString();
}

QString FusionJobsTest::publishedTrace(const QSignalSpy &dependencySpy, const QString &id)
{
    const QString available = availableIn(id);
    if (!available.isEmpty())
        return QStringLiteral("available: ") + available;
    if (spyHasAttribute(dependencySpy, id, kDiagnostics)
        || spyHasMeasurement(dependencySpy, id, QStringLiteral("Fusion"), QStringLiteral("roll")))
        return QStringLiteral("dependencyChanged was emitted for a fusion output");
    const std::optional<ResultStatus> status = engine(id).resultStatus(kFit);
    if (status.has_value() && *status != ResultStatus::NotRequested)
        return QStringLiteral("the fit has a cached result");
    if (engine(id).preparedCount() != 0)
        return QStringLiteral("a ticket is still outstanding");
    return QString();
}

void FusionJobsTest::onFirstProgress(JobId job, std::function<void()> action)
{
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = connect(m_queue.get(), &JobQueue::jobProgress, this,
                          [this, job, connection, action](JobId id, const QString &) {
        if (id != job)
            return;
        disconnect(*connection);
        action();
    });
}

// Acceptance 6: one job, all outputs together, the derived values with them,
// and nothing to do afterwards.
void FusionJobsTest::jobPublishesAllOutputsTogether()
{
    addSessions({fixtureSession(QStringLiteral("coarse_maneuver"), QStringLiteral("a")),
                 fixtureSession(QStringLiteral("coarse_linear"), QStringLiteral("b"))});
    QVERIFY2(availableIn("a").isEmpty(), qPrintable(availableIn("a")));    // read while unrequested
    QVERIFY2(availableIn("b").isEmpty(), qPrintable(availableIn("b")));
    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);

    const JobQueue::RequestResult result = m_queue->request("a", kFit);
    QCOMPARE(result.kind, Kind::Created);
    QCOMPARE(m_queue->job(result.job).state, JobState::Queued);
    QCOMPARE(m_queue->job(result.job).calculationTitle, QStringLiteral("Sensor fusion"));
    QCOMPARE(m_queue->request("a", kFit).kind, Kind::AlreadyActive);
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));

    const JobRecord job = m_queue->job(result.job);
    QCOMPARE(job.state, JobState::Succeeded);
    QVERIFY2(job.reason.isEmpty(), qPrintable(job.reason));
    QCOMPARE(job.resultStatus, std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(job.calculationId, kFit);
    QCOMPARE(job.calculationTitle, QStringLiteral("Sensor fusion"));
    QVERIFY(!job.progressText.isEmpty());
    QVERIFY(job.startedAt.isValid() && job.finishedAt.isValid());

    // Consumers are told to re-read every name they had read, in that session only
    for (const DependencyKey &name : fusionNames()) {
        if (name.type == DependencyKey::Type::Attribute)
            QVERIFY2(spyHasAttribute(dependencySpy, "a", name.attributeKey), qPrintable(name.attributeKey));
        else
            QVERIFY2(spyHasMeasurement(dependencySpy, "a", name.measurementKey.first, name.measurementKey.second),
                     qPrintable(name.measurementKey.second));
    }
    for (const QList<QVariant> &emission : dependencySpy)
        QCOMPARE(emission.at(0).toString(), QStringLiteral("a"));

    const QString difference = goldenDifference("a", QStringLiteral("coarse_maneuver"));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    const QString jsonDifference = compareJson(QStringLiteral("diagnostics"), diagnosticsOf("a"),
                                               loadFusionGolden(QStringLiteral("coarse_maneuver")).diagnostics);
    QVERIFY2(jsonDifference.isEmpty(), qPrintable(jsonDifference));

    // The derived values, never requested
    const qsizetype length = fusion("a", "_time").size();
    QVERIFY(length > 0);
    QCOMPARE(fusion("a", "accH").size(), length);
    QCOMPARE(fusion("a", "_system_time").size(), length);
    QVERIFY(session("a").getAttribute(fusionRollAtExit()).isValid());

    QCOMPARE(m_queue->request("a", kFit).kind, Kind::NothingToDo);
    QCOMPARE(engine("a").runCount(kFit), 1);
    QCOMPARE(engine("a").undeclaredReadCount(), 0);
    QCOMPARE(m_queue->model()->rowCount(), 1);

    // The other session is untouched
    QVERIFY2(availableIn("b").isEmpty(), qPrintable(availableIn("b")));
    QCOMPARE(engine("b").runCount(kFit), 0);
}

// Acceptance 7: the queue's worker and a synchronous request on the main
// thread give the same bits.
void FusionJobsTest::queueMatchesSynchronousRequest()
{
    addSessions({fixtureSession(QStringLiteral("stationary_spin"), QStringLiteral("a")),
                 fixtureSession(QStringLiteral("stationary_spin"), QStringLiteral("b"))});

    const JobQueue::RequestResult result = m_queue->request("a", kFit);
    QCOMPARE(result.kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));
    QCOMPARE(m_queue->job(result.job).state, JobState::Succeeded);

    QCOMPARE(engine("b").request(kFit).status, ResultStatus::Ok);

    QVERIFY(!fusion("a", "_time").isEmpty());
    for (const QString &name : fusionMeasurementNames())
        QVERIFY2(sameBitsEverywhere(fusion("a", name), fusion("b", name)), qPrintable(name));
    QVERIFY(sameBitsEverywhere(fusion("a", "accH"), fusion("b", "accH")));
    QVERIFY(sameBitsEverywhere(fusion("a", "_system_time"), fusion("b", "_system_time")));
    QVERIFY(session("a").getAttribute(kDiagnostics).isValid());
    QCOMPARE(session("a").getAttribute(kDiagnostics).toString(), session("b").getAttribute(kDiagnostics).toString());
    QVERIFY(engine("a").resultStatus(kFit) == engine("b").resultStatus(kFit));
    QCOMPARE(engine("a").resultDetail(kFit), engine("b").resultDetail(kFit));
}

// Acceptance 8, first half: a declared input changes while the fit runs. The
// job ends Superseded, nothing is published, and the fit can be asked again.
void FusionJobsTest::inputChangeDuringFitSupersedes()
{
    addSessions({fixtureSession(QStringLiteral("coarse_maneuver"), QStringLiteral("a"))});
    QVERIFY2(availableIn("a").isEmpty(), qPrintable(availableIn("a")));

    const JobQueue::RequestResult result = m_queue->request("a", kFit);
    QCOMPARE(result.kind, Kind::Created);
    bool edited = false;
    onFirstProgress(result.job, [this, &edited, job = result.job] {
        // The origin moves from fix 3 to fix 4 (both under 10 m)
        edited = m_queue->job(job).state == JobState::Running
            && m_model->updateAttribute("a", "_LOCAL_ORIGIN_INDEX", QVariant::fromValue(qlonglong(4)));
    });
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));
    QVERIFY(edited);

    const JobRecord job = m_queue->job(result.job);
    QCOMPARE(job.state, JobState::Superseded);
    QCOMPARE(job.reason, QStringLiteral("Inputs changed"));
    QVERIFY(!job.resultStatus.has_value());

    QVERIFY2(availableIn("a").isEmpty(), qPrintable(availableIn("a")));
    const std::optional<ResultStatus> status = engine("a").resultStatus(kFit);
    QVERIFY(!status.has_value() || *status == ResultStatus::NotRequested);
    QCOMPARE(engine("a").runCount(kFit), 0);        // a refused run is not a run of this engine
    QCOMPARE(engine("a").preparedCount(), 0);
    QVERIFY(engine("a").readiness(kFit).state == ReadyState::Ready);

    // The queue does not ask again; whoever wants the result does
    QVERIFY(m_queue->isIdle());
    const JobQueue::RequestResult again = m_queue->request("a", kFit);
    QCOMPARE(again.kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));
    QCOMPARE(m_queue->job(again.job).state, JobState::Succeeded);
    QVERIFY2(m_queue->job(again.job).reason.isEmpty(), qPrintable(m_queue->job(again.job).reason));
    QCOMPARE(diagnosticsOf("a").value(QStringLiteral("input")).toObject().value(QStringLiteral("origin_index")).toInt(-1), 4);
    QVERIFY(!fusion("a", "roll").isEmpty());
    QCOMPARE(engine("a").runCount(kFit), 1);
}

// Acceptance 9: a recording the model rejects is a succeeded job that carries
// the reason; asking again does nothing; new inputs give a fresh run.
void FusionJobsTest::rejectedRecordingIsSucceededJob()
{
    addSessions({fixtureSession(QStringLiteral("reject_origin"), QStringLiteral("a"))});
    const QString failure = QStringLiteral("Local origin index outside GNSS samples");
    QCOMPARE(loadFusionGolden(QStringLiteral("reject_origin")).diagnostics.value(QStringLiteral("failure")).toString(),
             failure);

    const JobQueue::RequestResult result = m_queue->request("a", kFit);
    QCOMPARE(result.kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));

    const JobRecord job = m_queue->job(result.job);
    QCOMPARE(job.state, JobState::Succeeded);
    QCOMPARE(job.resultStatus, std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(job.reason, failure);

    QVERIFY(session("a").getAttribute(kDiagnostics).isValid());
    QCOMPARE(diagnosticsOf("a").value(QStringLiteral("failure")).toString(), failure);
    for (const QString &name : fusionMeasurementNames())
        QVERIFY2(fusion("a", name).isEmpty(), qPrintable(name));
    QVERIFY(fusion("a", "accH").isEmpty());
    QVERIFY(engine("a").blockers(fusionKey(QStringLiteral("roll"))).state == BlockerState::NotProduced);

    QCOMPARE(m_queue->request("a", kFit).kind, Kind::NothingToDo);
    QCOMPARE(engine("a").runCount(kFit), 1);
    QCOMPARE(m_queue->model()->rowCount(), 1);

    // The input changes: reject_origin is coarse_linear with origin index 9
    QVERIFY(m_model->updateAttribute("a", "_LOCAL_ORIGIN_INDEX", QVariant::fromValue(qlonglong(0))));
    QVERIFY(!session("a").getAttribute(kDiagnostics).isValid());
    const JobQueue::RequestResult again = m_queue->request("a", kFit);
    QCOMPARE(again.kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));
    QCOMPARE(m_queue->job(again.job).state, JobState::Succeeded);
    QVERIFY2(m_queue->job(again.job).reason.isEmpty(), qPrintable(m_queue->job(again.job).reason));
    const QString difference = goldenDifference("a", QStringLiteral("coarse_linear"));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    QCOMPARE(engine("a").runCount(kFit), 2);
}

// Acceptance 10: cancelling the running fit publishes nothing, leaves it
// requestable, and lets the next job start.
void FusionJobsTest::cancelDuringFitThenNextJobStarts()
{
    addSessions({fixtureSession(QStringLiteral("stationary_spin"), QStringLiteral("s1")),
                 fixtureSession(QStringLiteral("coarse_linear"), QStringLiteral("s2"))});
    QVERIFY2(availableIn("s1").isEmpty(), qPrintable(availableIn("s1")));
    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);

    const JobId job1 = m_queue->request("s1", kFit).job;
    const JobId job2 = m_queue->request("s2", kFit).job;
    QVERIFY(job1 != 0 && job2 != 0);

    QElapsedTimer sinceCancel;
    qint64 cancelToEndMs = -1;
    bool cancelled = false;
    onFirstProgress(job1, [&] {
        sinceCancel.start();
        cancelled = m_queue->cancel(job1);
    });
    connect(m_queue.get(), &JobQueue::jobFinished, this, [&](JobId id, JobState) {
        if (id == job1 && sinceCancel.isValid())
            cancelToEndMs = sinceCancel.elapsed();
    });
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));
    QVERIFY(cancelled);

    const JobRecord first = m_queue->job(job1);
    QCOMPARE(first.state, JobState::Cancelled);
    QCOMPARE(first.reason, QStringLiteral("Cancelled"));
    QVERIFY(!first.resultStatus.has_value());
    // Evidence of a stop at a boundary, not asserted: if the worker had
    // already returned when the cancel was processed, the job still ends
    // Cancelled (cancel wins). The boundary stop itself is proven in
    // tst_fusion_session::cancelStopsAtNextBoundary and in tst_fusion_parity.
    qInfo().noquote() << "cancel to job end:" << cancelToEndMs << "ms; last progress text:" << first.progressText;

    QCOMPARE(publishedTrace(dependencySpy, "s1"), QString());
    QVERIFY(engine("s1").readiness(kFit).state == ReadyState::Ready);
    QCOMPARE(engine("s1").runCount(kFit), 0);

    // The next job started only after the cancelled one had ended
    const JobRecord second = m_queue->job(job2);
    QCOMPARE(second.state, JobState::Succeeded);
    QVERIFY(second.startedAt >= first.finishedAt);
    QString difference = goldenDifference("s2", QStringLiteral("coarse_linear"));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));

    // Asked again, it runs to the end
    const JobQueue::RequestResult again = m_queue->request("s1", kFit);
    QCOMPARE(again.kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));
    QCOMPARE(m_queue->job(again.job).state, JobState::Succeeded);
    difference = goldenDifference("s1", QStringLiteral("stationary_spin"));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
}

// Acceptance 11: no IMU data, no job - and nothing to show for any fusion plot.
void FusionJobsTest::noImuSessionCannotHaveAJob()
{
    addSessions({sessionWithoutImu(fusionFixture(QStringLiteral("coarse_linear")), QStringLiteral("a"))});

    const JobQueue::RequestResult result = m_queue->request("a", kFit);
    QCOMPARE(result.kind, Kind::MissingInput);
    QCOMPARE(result.job, JobId(0));
    QCOMPARE(m_queue->model()->rowCount(), 0);
    QVERIFY(m_queue->isIdle());
    QVERIFY(!m_model->isSessionPinned("a"));

    QStringList plots = fusionMeasurementNames();
    plots.removeAll(QStringLiteral("_time"));
    plots.append(QStringLiteral("accH"));
    QCOMPARE(plots.size(), 17);
    for (const QString &name : std::as_const(plots)) {
        const BlockerReport report = engine("a").blockers(fusionKey(name));
        QVERIFY2(report.state == BlockerState::NotApplicable, qPrintable(name));
        QVERIFY(report.blockers.isEmpty());
    }
    QCOMPARE(engine("a").runCount(kFit), 0);
}

// Acceptance 5, at model level: the logbook column over a fusion output, the
// column worker and the saver are readers. None of them starts a fit.
void FusionJobsTest::readersNeverStartAFit()
{
    QSignalSpy queuedSpy(m_queue.get(), &JobQueue::jobQueued);
    addSessions({fixtureSession(QStringLiteral("coarse_linear"), QStringLiteral("a")),
                 fixtureSession(QStringLiteral("coarse_maneuver"), QStringLiteral("b")),
                 naturalSession(QStringLiteral("c"))});

    // The view's reads, repeatedly
    for (int round = 0; round < 3; ++round) {
        for (int row = 0; row < m_model->rowCount(); ++row) {
            const QVariant cell = m_model->data(m_model->index(row, kRollColumn), Qt::DisplayRole);
            QVERIFY2(cell.toString().isEmpty(), qPrintable(cell.toString()));
        }
    }
    QVERIFY(waitForIdle(*m_model));

    QCOMPARE(queuedSpy.count(), 0);
    QCOMPARE(m_queue->model()->rowCount(), 0);
    QVERIFY(m_queue->isIdle());
    for (const char *id : {"a", "b", "c"}) {
        QCOMPARE(engine(id).runCount(kFit), 0);
        QCOMPARE(engine(id).preparedCount(), 0);
        QVERIFY(!m_model->isSessionPinned(id));
        const int row = m_model->getSessionRow(id);
        QVERIFY(std::as_const(*m_model).rowAt(row).cachedValues.contains(kRollColumn));
        QVERIFY(!std::as_const(*m_model).rowAt(row).cachedValues.value(kRollColumn).isValid());
    }
}

// An explicit result is never saved, so the column's cached value - its value
// for the session as it is on disk - is "unavailable" before and after a
// published fit. The loaded row shows the live number.
void FusionJobsTest::columnOnFusionOutputIsNotCached()
{
    const LogbookColumn column = rollColumn();
    addSessions({fixtureSession(QStringLiteral("coarse_linear"), QStringLiteral("a"))});
    const int row = m_model->getSessionRow("a");
    const auto cachedRoll = [this, row] { return std::as_const(*m_model).rowAt(row).cachedValues; };

    QVERIFY(cachedRoll().contains(kRollColumn));
    QVERIFY(!cachedRoll().value(kRollColumn).isValid());
    QVERIFY(!indexValue("a", column).isDouble());

    const JobQueue::RequestResult result = m_queue->request("a", kFit);
    QCOMPARE(result.kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));
    QCOMPARE(m_queue->job(result.job).state, JobState::Succeeded);
    QVERIFY(waitForIdle(*m_model));

    // Publication leaves the cached value alone
    QVERIFY(cachedRoll().contains(kRollColumn));
    QVERIFY(!cachedRoll().value(kRollColumn).isValid());

    // A marker edit makes the column worker compute the column again, with
    // the fit published (markers are not inputs of the fit)
    QVERIFY(m_model->updateAttribute("a", "_EXIT_TIME", kExitTime + .25));
    QVERIFY(!cachedRoll().contains(kRollColumn));
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(engine("a").runCount(kFit), 1);
    QVERIFY(session("a").getAttribute(fusionRollAtExit()).isValid());

    bool isNumber = false;
    m_model->data(m_model->index(row, kRollColumn), Qt::DisplayRole).toString().toDouble(&isNumber);
    QVERIFY(isNumber);
    QVERIFY(cachedRoll().contains(kRollColumn));
    QVERIFY(!cachedRoll().value(kRollColumn).isValid());
    QVERIFY(!LogbookManager::instance().hasUnsavedColumns("a"));
    QVERIFY(!indexValue("a", column).isDouble());
    QVERIFY(!indexValue("a", column).isString());
}

// Quitting while a fit runs: shutdown returns, nothing is published, nothing
// crashes when the queue and then the model go away.
void FusionJobsTest::shutdownDuringFit()
{
    addSessions({fixtureSession(QStringLiteral("stationary_spin"), QStringLiteral("s1")),
                 fixtureSession(QStringLiteral("coarse_linear"), QStringLiteral("s2"))});
    QVERIFY2(availableIn("s1").isEmpty(), qPrintable(availableIn("s1")));
    QVERIFY2(availableIn("s2").isEmpty(), qPrintable(availableIn("s2")));
    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);

    const JobId running = m_queue->request("s1", kFit).job;
    const JobId queued = m_queue->request("s2", kFit).job;
    QVERIFY(running != 0 && queued != 0);

    // The fit is running when its first progress text arrives
    bool wasRunning = false;
    QElapsedTimer timer;
    qint64 shutdownMs = -1;
    onFirstProgress(running, [&] {
        wasRunning = m_queue->job(running).state == JobState::Running;
        timer.start();
        m_queue->shutdown();
        shutdownMs = timer.elapsed();
    });
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));
    QVERIFY(wasRunning);
    QVERIFY(m_queue->isShutDown());
    qInfo() << "shutdown during the fit returned after" << shutdownMs << "ms";

    for (const JobId id : {running, queued}) {
        QCOMPARE(m_queue->job(id).state, JobState::Cancelled);
        QCOMPARE(m_queue->job(id).reason, QStringLiteral("Application closing"));
    }
    QVERIFY(!m_queue->job(queued).startedAt.isValid());
    QCOMPARE(m_queue->request("s1", kFit).kind, Kind::ShuttingDown);

    // Late queued events (progress posts, the worker's end) find nothing
    QCoreApplication::processEvents();
    QCOMPARE(m_queue->job(running).state, JobState::Cancelled);
    QCOMPARE(publishedTrace(dependencySpy, "s1"), QString());
    QCOMPARE(publishedTrace(dependencySpy, "s2"), QString());
    QVERIFY(!m_model->isSessionPinned("s1"));
    QVERIFY(!m_model->isSessionPinned("s2"));

    m_queue.reset();
    m_model.reset();
}

// Optional local check on a real recording; not a CI test (tests/README.md).
// FLYSIGHT_FUSION_RECORDING names a folder with TRACK.CSV and SENSOR.CSV. Run
// the executable directly: CTest's timeout is shorter than a long fit.
void FusionJobsTest::realRecordingCheck()
{
    const QString folder = qEnvironmentVariable("FLYSIGHT_FUSION_RECORDING");
    if (folder.isEmpty())
        QSKIP("FLYSIGHT_FUSION_RECORDING is not set");
    const QString track = folder + QStringLiteral("/TRACK.CSV");
    const QString sensor = folder + QStringLiteral("/SENSOR.CSV");
    if (!QFileInfo::exists(track) || !QFileInfo::exists(sensor))
        QSKIP("FLYSIGHT_FUSION_RECORDING has no TRACK.CSV and SENSOR.CSV");

    const SessionImport::BatchResult imported = SessionImport::importFiles(*m_model, {track, sensor});
    QVERIFY2(imported.failures().isEmpty(), qPrintable(SessionImport::failureMessage(imported.failures(), folder)));
    QCOMPARE(imported.importedSessionIds().size(), 1);
    const QString id = imported.importedSessionIds().first();
    QVERIFY(m_model->loadedSession(id) != nullptr);

    QElapsedTimer timer;
    timer.start();
    const JobQueue::RequestResult result = m_queue->request(id, kFit);
    QCOMPARE(result.kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue, 30 * 60 * 1000));
    const JobRecord job = m_queue->job(result.job);
    QCOMPARE(job.state, JobState::Succeeded);
    QVERIFY2(job.reason.isEmpty(), qPrintable(job.reason));

    const QVector<double> time = fusion(id, "_time");
    const QVector<double> imuTime = session(id).getMeasurement("IMU", "_time");
    QVERIFY(!time.isEmpty());
    for (const QString &name : fusionMeasurementNames())
        QCOMPARE(fusion(id, name).size(), time.size());
    const qsizetype first = imuTime.indexOf(time.first());
    QVERIFY(first >= 0);
    QVERIFY(sameBitsEverywhere(time, imuTime.mid(first, time.size())));

    const QJsonObject diagnostics = diagnosticsOf(id);
    qInfo().noquote() << "elapsed" << timer.elapsed() / 1000.0 << "s;"
                      << "objective" << QString::number(diagnostics.value(QStringLiteral("objective")).toDouble(), 'g', 17)
                      << "gnss_states" << diagnostics.value(QStringLiteral("gnss_states")).toInt()
                      << "imu_outputs" << diagnostics.value(QStringLiteral("imu_outputs")).toInt();
    qInfo().noquote() << "diagnostics:" << session(id).getAttribute(kDiagnostics).toString().left(2000);
}

FLYSIGHT_TEST_MAIN(FusionJobsTest)
#include "tst_fusion_jobs.moc"
