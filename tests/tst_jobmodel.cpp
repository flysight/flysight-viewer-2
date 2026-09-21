// The job model's contract (sensor-fusion-jobs acceptance 18): every
// transition arrives through model signals, finished jobs are retained with
// state, timing and reason, and a view can render the whole history from the
// model alone. Driven by a real JobQueue with the synthetic calculations of
// jobfixture.h; a QAbstractItemModelTester watches every function.

#include <memory>

#include <QAbstractItemModelTester>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QLocale>
#include <QMap>
#include <QSettings>
#include <QSignalSpy>
#include <QtTest>

#include "engine/calculationregistry.h"
#include "jobfixture.h"
#include "jobmodel.h"
#include "jobqueue.h"
#include "logbookcolumn.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "sessiondata.h"
#include "sessionmodel.h"
#include "testenvironment.h"
#include "testmain.h"

using namespace FlySight;
using namespace FlySightTest;

namespace {

const QList<int> kJobRoles = {
    JobModel::JobIdRole, JobModel::SessionIdRole, JobModel::SessionNameRole,
    JobModel::CalculationIdRole, JobModel::InstanceIdRole, JobModel::CalculationTitleRole,
    JobModel::StateRole, JobModel::CancelRequestedRole, JobModel::ProgressTextRole,
    JobModel::QueuedTimeRole, JobModel::StartedTimeRole, JobModel::FinishedTimeRole,
    JobModel::ReasonRole, JobModel::ResultStatusRole, JobModel::IsFinishedRole};

/// A jobs view as a dock would be written: it is given a QAbstractItemModel
/// and nothing else, listens to the standard model signals, and reads through
/// index() / data() with the role and column enum values only.
class HistoryView : public QObject {
public:
    using Row = QMap<int, QVariant>;    // role -> value; Qt::DisplayRole + column for the texts

    explicit HistoryView(QAbstractItemModel *model)
        : m_model(model)
    {
        connect(model, &QAbstractItemModel::rowsInserted, this,
                [this](const QModelIndex &, int first, int last) {
            for (int row = first; row <= last; ++row) {
                m_rows.insert(row, read(row));
                noteState(m_rows.at(row));
            }
            afterSignal();
        });
        connect(model, &QAbstractItemModel::rowsRemoved, this,
                [this](const QModelIndex &, int first, int last) {
            m_rows.remove(first, last - first + 1);
            afterSignal();
        });
        connect(model, &QAbstractItemModel::dataChanged, this,
                [this](const QModelIndex &topLeft, const QModelIndex &bottomRight, const QList<int> &roles) {
            for (int row = topLeft.row(); row <= bottomRight.row(); ++row) {
                m_rows[row] = read(row);
                noteState(m_rows.at(row));
                const bool progressOnly = topLeft.column() == JobModel::ProgressColumn
                                          && bottomRight.column() == JobModel::ProgressColumn;
                if (progressOnly && roles.contains(JobModel::ProgressTextRole))
                    m_progress[jobId(m_rows.at(row))].append(m_rows.at(row).value(JobModel::ProgressTextRole).toString());
            }
            afterSignal();
        });
        connect(model, &QAbstractItemModel::modelReset, this, [this] { ++m_resets; });
        connect(model, &QAbstractItemModel::layoutChanged, this, [this] { ++m_resets; });
    }

    int rowCount() const { return int(m_rows.size()); }
    Row row(int i) const { return m_rows.value(i); }
    Row rowOfJob(qulonglong id) const
    {
        for (const Row &r : m_rows) {
            if (jobId(r) == id)
                return r;
        }
        return Row();
    }
    /// The distinct states a job went through, as the signals showed them.
    QList<int> states(qulonglong id) const { return m_states.value(id); }
    QStringList progressTexts(qulonglong id) const { return m_progress.value(id); }
    int maxRunningRows() const { return m_maxRunning; }
    int resets() const { return m_resets; }
    /// The display text of one cell of the mirror.
    static QString text(const Row &r, int column) { return r.value(-1 - column).toString(); }

private:
    static qulonglong jobId(const Row &r) { return r.value(JobModel::JobIdRole).toULongLong(); }

    Row read(int row) const
    {
        Row r;
        const QModelIndex first = m_model->index(row, 0);
        for (const int role : kJobRoles)
            r.insert(role, first.data(role));
        for (int column = 0; column < m_model->columnCount(); ++column)
            r.insert(-1 - column, m_model->index(row, column).data(Qt::DisplayRole));
        return r;
    }

    void noteState(const Row &r)
    {
        QList<int> &seen = m_states[jobId(r)];
        const int state = r.value(JobModel::StateRole).toInt();
        if (seen.isEmpty() || seen.last() != state)
            seen.append(state);
    }

    void afterSignal()
    {
        int running = 0;
        for (const Row &r : std::as_const(m_rows)) {
            if (r.value(JobModel::StateRole).toInt() == int(JobState::Running))
                ++running;
        }
        m_maxRunning = qMax(m_maxRunning, running);
    }

    QAbstractItemModel *m_model;
    QList<Row> m_rows;
    QMap<qulonglong, QList<int>> m_states;
    QMap<qulonglong, QStringList> m_progress;
    int m_maxRunning = 0;
    int m_resets = 0;
};

constexpr int Q = int(JobState::Queued);
constexpr int R = int(JobState::Running);
constexpr int kSucceeded = int(JobState::Succeeded);
constexpr int kCancelled = int(JobState::Cancelled);
constexpr int kSuperseded = int(JobState::Superseded);
constexpr int kFailed = int(JobState::Failed);

/// Relative path -> bytes of every file under `root`.
QMap<QString, QByteArray> snapshot(const QString &root)
{
    QMap<QString, QByteArray> files;
    QDirIterator it(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        QFile file(path);
        if (file.open(QIODevice::ReadOnly))
            files.insert(QDir(root).relativeFilePath(path), file.readAll());
    }
    return files;
}

} // namespace

class JobModelTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void rolesAndColumns();
    void historyFromSignalsAlone();
    void neverMoreThanOneRunningRow();
    void timestampsAreOrdered();
    void progressIsItsOwnSignal();
    void cancelRequestedIsVisible();
    void removeFinishedAndClear();
    void removeRowsRefusesActive();
    void retentionBound();
    void nothingIsPersisted();

private:
    void setInput(const QString &sessionId, const char *key, int value)
    {
        QVERIFY(m_sessions->updateAttribute(sessionId, QString::fromLatin1(key), value));
    }
    JobId request(const QString &sessionId, const char *calculationId)
    {
        return m_queue->request(sessionId, QString::fromLatin1(calculationId)).job;
    }
    /// request() and cancel() while it is still queued: a finished row at once.
    JobId cancelledJob(const QString &sessionId, const char *calculationId)
    {
        const JobId id = request(sessionId, calculationId);
        m_queue->cancel(id);
        return id;
    }
    Gate &gate() { return m_world->gate(); }
    JobModel *model() const { return m_queue->model(); }

    std::unique_ptr<JobWorld> m_world;
    std::unique_ptr<SessionModel> m_sessions;
    std::unique_ptr<JobQueue> m_queue;
    std::unique_ptr<QAbstractItemModelTester> m_tester;
    std::unique_ptr<HistoryView> m_view;
    std::unique_ptr<QSignalSpy> m_resetSpy;
    QStringList m_registryBefore;
};

void JobModelTest::initTestCase()
{
    TestEnvironment::instance().registerBuiltIns();

    PreferencesManager::instance().registerPreference(PreferenceKeys::LogbookColumnsVersion, 0);
    LogbookColumn description;
    description.type = ColumnType::SessionAttribute;
    description.attributeKey = QString::fromLatin1(SessionKeys::Description);
    LogbookColumnStore::instance().setColumns({description});
}

void JobModelTest::init()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();
    env.resetPreferencesToDefaults();
    m_registryBefore = CalculationRegistry::instance().registeredIds();

    m_world = std::make_unique<JobWorld>();
    m_sessions = std::make_unique<SessionModel>();
    m_sessions->mergeSessions(JobWorld::sessions({"s1", "s2", "s3"}));
    QCOMPARE(m_sessions->rowCount(), 3);
    m_sessions->flushPendingInvalidations();

    m_queue = std::make_unique<JobQueue>(m_sessions.get());
    m_tester = std::make_unique<QAbstractItemModelTester>(
        model(), QAbstractItemModelTester::FailureReportingMode::QtTest);
    m_view = std::make_unique<HistoryView>(model());
    m_resetSpy = std::make_unique<QSignalSpy>(model(), &QAbstractItemModel::modelReset);
}

void JobModelTest::cleanup()
{
    if (m_queue) {
        m_queue->shutdown();
        // No modelReset at any time after construction
        QCOMPARE(m_resetSpy->count(), 0);
        QCOMPARE(m_view->resets(), 0);
        QVERIFY(m_view->maxRunningRows() <= 1);
        QCOMPARE(m_view->rowCount(), model()->rowCount());
    }
    m_resetSpy.reset();
    m_view.reset();
    m_tester.reset();
    m_queue.reset();
    m_sessions.reset();
    m_world.reset();
    QCOMPARE(CalculationRegistry::instance().registeredIds(), m_registryBefore);
    QCOMPARE(CalculationRegistry::instance().enrolledEngineCount(), 0);
}

void JobModelTest::rolesAndColumns()
{
    QCOMPARE(model()->columnCount(), 8);
    QCOMPARE(int(JobModel::ColumnCount), 8);
    QCOMPARE(model()->rowCount(), 0);
    QCOMPARE(model()->rowCount(model()->index(0, 0)), 0);

    const QStringList headers = {"Session", "Calculation", "State", "Progress",
                                 "Queued", "Started", "Finished", "Reason"};
    for (int column = 0; column < headers.size(); ++column) {
        QCOMPARE(model()->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString(), headers.at(column));
        QVERIFY(!model()->headerData(column, Qt::Horizontal, Qt::DecorationRole).isValid());
    }
    QVERIFY(!model()->headerData(8, Qt::Horizontal, Qt::DisplayRole).isValid());

    const QHash<int, QByteArray> names = model()->roleNames();
    QCOMPARE(names.value(JobModel::JobIdRole), QByteArray("jobId"));
    QCOMPARE(names.value(JobModel::SessionIdRole), QByteArray("sessionId"));
    QCOMPARE(names.value(JobModel::SessionNameRole), QByteArray("sessionName"));
    QCOMPARE(names.value(JobModel::CalculationIdRole), QByteArray("calculationId"));
    QCOMPARE(names.value(JobModel::InstanceIdRole), QByteArray("instanceId"));
    QCOMPARE(names.value(JobModel::CalculationTitleRole), QByteArray("calculationTitle"));
    QCOMPARE(names.value(JobModel::StateRole), QByteArray("state"));
    QCOMPARE(names.value(JobModel::CancelRequestedRole), QByteArray("cancelRequested"));
    QCOMPARE(names.value(JobModel::ProgressTextRole), QByteArray("progressText"));
    QCOMPARE(names.value(JobModel::QueuedTimeRole), QByteArray("queuedTime"));
    QCOMPARE(names.value(JobModel::StartedTimeRole), QByteArray("startedTime"));
    QCOMPARE(names.value(JobModel::FinishedTimeRole), QByteArray("finishedTime"));
    QCOMPARE(names.value(JobModel::ReasonRole), QByteArray("reason"));
    QCOMPARE(names.value(JobModel::ResultStatusRole), QByteArray("resultStatus"));
    QCOMPARE(names.value(JobModel::IsFinishedRole), QByteArray("isFinished"));

    QCOMPARE(JobModel::stateText(JobState::Queued), QStringLiteral("Queued"));
    QCOMPARE(JobModel::stateText(JobState::Running), QStringLiteral("Running"));
    QCOMPARE(JobModel::stateText(JobState::Succeeded), QStringLiteral("Succeeded"));
    QCOMPARE(JobModel::stateText(JobState::Cancelled), QStringLiteral("Cancelled"));
    QCOMPARE(JobModel::stateText(JobState::Superseded), QStringLiteral("Superseded"));
    QCOMPARE(JobModel::stateText(JobState::Failed), QStringLiteral("Failed"));

    // A queued job: every role on every column
    QVERIFY(m_sessions->updateAttribute("s1", "_DESCRIPTION", QStringLiteral("First jump")));
    setInput("s1", "EA_IN", -1);
    const JobId id = request("s1", "expA");
    QCOMPARE(id, JobId(1));
    QCOMPARE(model()->rowCount(), 1);
    QCOMPARE(model()->rowOf(id), 0);
    QCOMPARE(model()->rowOf(JobId(7)), -1);
    QCOMPARE(model()->record(JobId(7)).id, JobId(0));
    QCOMPARE(model()->record(5).id, JobId(0));

    for (int column = 0; column < JobModel::ColumnCount; ++column) {
        const QModelIndex cell = model()->index(0, column);
        QCOMPARE(model()->flags(cell), Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        QCOMPARE(cell.data(JobModel::JobIdRole).toULongLong(), 1ULL);
        QCOMPARE(cell.data(JobModel::SessionIdRole).toString(), QStringLiteral("s1"));
        QCOMPARE(cell.data(JobModel::SessionNameRole).toString(), QStringLiteral("First jump"));
        QCOMPARE(cell.data(JobModel::CalculationIdRole).toString(), QStringLiteral("expA"));
        QCOMPARE(cell.data(JobModel::InstanceIdRole).toString(), QStringLiteral("expA"));
        QCOMPARE(cell.data(JobModel::CalculationTitleRole).toString(), QStringLiteral("Explicit A"));
        QCOMPARE(cell.data(JobModel::StateRole), QVariant(int(JobState::Queued)));
        QCOMPARE(cell.data(JobModel::CancelRequestedRole), QVariant(false));
        QCOMPARE(cell.data(JobModel::ProgressTextRole).toString(), QString());
        QVERIFY(cell.data(JobModel::QueuedTimeRole).toDateTime().isValid());
        QCOMPARE(cell.data(JobModel::StartedTimeRole).typeId(), int(QMetaType::QDateTime));
        QVERIFY(!cell.data(JobModel::StartedTimeRole).toDateTime().isValid());
        QVERIFY(!cell.data(JobModel::FinishedTimeRole).toDateTime().isValid());
        QCOMPARE(cell.data(JobModel::ReasonRole).toString(), QString());
        QVERIFY(!cell.data(JobModel::ResultStatusRole).isValid());
        QCOMPARE(cell.data(JobModel::IsFinishedRole), QVariant(false));
        QVERIFY(!cell.data(Qt::DecorationRole).isValid());
    }
    QCOMPARE(model()->index(0, JobModel::StateColumn).data().toString(), QStringLiteral("Queued"));
    QCOMPARE(model()->index(0, JobModel::StartedColumn).data().toString(), QString());
    QCOMPARE(model()->index(0, JobModel::FinishedColumn).data().toString(), QString());
    QCOMPARE(model()->flags(QModelIndex()), Qt::NoItemFlags);
    QVERIFY(!model()->data(QModelIndex(), JobModel::JobIdRole).isValid());
    QVERIFY(!model()->data(model()->index(1, 0), JobModel::JobIdRole).isValid());
    QVERIFY(!model()->data(model()->index(0, 8), Qt::DisplayRole).isValid());

    // Finished (a rejection): the display texts and the outcome roles
    QVERIFY(waitIdle(*m_queue));
    const JobRecord job = m_queue->job(id);
    const QLocale locale;
    const QStringList expected = {
        QStringLiteral("First jump"), QStringLiteral("Explicit A"), QStringLiteral("Succeeded"), QString(),
        locale.toString(job.queuedAt.toLocalTime(), QLocale::ShortFormat),
        locale.toString(job.startedAt.toLocalTime(), QLocale::ShortFormat),
        locale.toString(job.finishedAt.toLocalTime(), QLocale::ShortFormat),
        QStringLiteral("negative input")};
    for (int column = 0; column < JobModel::ColumnCount; ++column) {
        const QModelIndex cell = model()->index(0, column);
        QCOMPARE(cell.data(Qt::DisplayRole).toString(), expected.at(column));
        QCOMPARE(cell.data(JobModel::StateRole), QVariant(int(JobState::Succeeded)));
        QCOMPARE(cell.data(JobModel::ReasonRole).toString(), QStringLiteral("negative input"));
        QCOMPARE(cell.data(JobModel::ResultStatusRole), QVariant(int(ResultStatus::Ok)));
        QCOMPARE(cell.data(JobModel::IsFinishedRole), QVariant(true));
        QCOMPARE(cell.data(JobModel::StartedTimeRole).toDateTime(), job.startedAt);
        QCOMPARE(cell.data(JobModel::FinishedTimeRole).toDateTime(), job.finishedAt);
    }
    QVERIFY(!expected.at(JobModel::FinishedColumn).isEmpty());
}

// Acceptance 18: a scripted session with every kind of ending, rendered by a
// view that knows the model only.
void JobModelTest::historyFromSignalsAlone()
{
    setInput("s1", "G_IN", 4);
    setInput("s1", "T_IN", 4);
    setInput("s1", "X_IN", 4);
    setInput("s1", "EA_IN", 4);
    setInput("s2", "EA_IN", -1);
    setInput("s2", "G_IN", 6);
    setInput("s3", "G_IN", 3);
    setInput("s3", "EA_IN", 4);

    // 1. success, with two progress texts
    const JobId success = request("s1", "gated");
    QVERIFY(gate().waitEntered());
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));

    // 2. rejection   3. exception (a published failure)
    const JobId rejection = request("s2", "expA");
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("synthetic failure")));
    const JobId exception = request("s1", "thrower");
    QVERIFY(waitIdle(*m_queue));

    // 4. cancel while running
    const JobId cancelRunning = request("s2", "gated");
    QVERIFY(gate().waitEntered());
    QVERIFY(m_queue->cancel(cancelRunning));
    QVERIFY(waitIdle(*m_queue));

    // 5. cancel while queued   6. superseded by an input change while running
    const JobId supersededRunning = request("s3", "gated");
    QVERIFY(gate().waitEntered());
    const JobId cancelQueued = request("s3", "expA");
    QVERIFY(m_queue->cancel(cancelQueued));
    setInput("s3", "G_IN", 5);
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));

    // 7. superseded at start: its input goes away while it waits
    const JobId holder = request("s3", "gated");
    QVERIFY(gate().waitEntered());
    const JobId supersededAtStart = request("s3", "expA");
    QVERIFY(m_sessions->removeAttribute("s3", "EA_IN"));
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));

    // 8. failed: out of memory   9. failed: the worker could not start
    const JobId exhausted = request("s1", "exhausted");
    QVERIFY(waitIdle(*m_queue));
    m_queue->failNextWorkerStarts(1);
    const JobId noWorker = request("s1", "expA");
    QVERIFY(waitIdle(*m_queue));

    QCOMPARE(model()->rowCount(), 10);

    // The state sequences, as separate signals
    QCOMPARE(m_view->states(success), QList<int>({Q, R, kSucceeded}));
    QCOMPARE(m_view->states(rejection), QList<int>({Q, R, kSucceeded}));
    QCOMPARE(m_view->states(exception), QList<int>({Q, R, kSucceeded}));
    QCOMPARE(m_view->states(cancelRunning), QList<int>({Q, R, kCancelled}));
    QCOMPARE(m_view->states(supersededRunning), QList<int>({Q, R, kSuperseded}));
    QCOMPARE(m_view->states(cancelQueued), QList<int>({Q, kCancelled}));
    QCOMPARE(m_view->states(holder), QList<int>({Q, R, kSucceeded}));
    QCOMPARE(m_view->states(supersededAtStart), QList<int>({Q, kSuperseded}));
    QCOMPARE(m_view->states(exhausted), QList<int>({Q, R, kFailed}));
    QCOMPARE(m_view->states(noWorker), QList<int>({Q, R, kFailed}));
    QCOMPARE(m_view->progressTexts(success), QStringList({"step 1", "step 2"}));
    QCOMPARE(m_view->progressTexts(rejection), QStringList());
    QCOMPARE(m_view->maxRunningRows(), 1);

    // What the view would paint for the outcome of each job
    const auto outcome = [this](JobId id) {
        const HistoryView::Row r = m_view->rowOfJob(id);
        return QStringList({HistoryView::text(r, JobModel::StateColumn), HistoryView::text(r, JobModel::ReasonColumn)});
    };
    QCOMPARE(outcome(success), QStringList({"Succeeded", ""}));
    QCOMPARE(outcome(rejection), QStringList({"Succeeded", "negative input"}));
    QCOMPARE(outcome(exception), QStringList({"Succeeded", "Calculation failed: synthetic failure"}));
    QCOMPARE(outcome(cancelRunning), QStringList({"Cancelled", "Cancelled"}));
    QCOMPARE(outcome(supersededRunning), QStringList({"Superseded", "Inputs changed"}));
    QCOMPARE(outcome(cancelQueued), QStringList({"Cancelled", "Cancelled"}));
    QCOMPARE(outcome(supersededAtStart), QStringList({"Superseded", "Inputs changed: nothing to compute"}));
    QCOMPARE(outcome(exhausted), QStringList({"Failed", "Out of memory"}));
    QCOMPARE(outcome(noWorker), QStringList({"Failed", "The worker thread could not be started"}));
    QCOMPARE(m_view->rowOfJob(exception).value(JobModel::ResultStatusRole), QVariant(int(ResultStatus::Failed)));
    QCOMPARE(m_view->rowOfJob(success).value(JobModel::ResultStatusRole), QVariant(int(ResultStatus::Ok)));
    QCOMPARE(HistoryView::text(m_view->rowOfJob(success), JobModel::ProgressColumn), QStringLiteral("step 2"));
    QCOMPARE(HistoryView::text(m_view->rowOfJob(success), JobModel::CalculationColumn), QStringLiteral("Gated"));

    // The mirror, built from signals alone, equals the queue's records
    QCOMPARE(m_view->rowCount(), model()->rowCount());
    for (int i = 0; i < m_view->rowCount(); ++i) {
        const HistoryView::Row r = m_view->row(i);
        const JobRecord job = m_queue->job(r.value(JobModel::JobIdRole).toULongLong());
        QCOMPARE(job.id, JobId(i + 1));
        QCOMPARE(r.value(JobModel::SessionIdRole).toString(), job.sessionId);
        QCOMPARE(r.value(JobModel::SessionNameRole).toString(), job.sessionName);
        QCOMPARE(r.value(JobModel::CalculationIdRole).toString(), job.calculationId);
        QCOMPARE(r.value(JobModel::InstanceIdRole).toString(), job.instanceId);
        QCOMPARE(r.value(JobModel::CalculationTitleRole).toString(), job.calculationTitle);
        QCOMPARE(r.value(JobModel::StateRole).toInt(), int(job.state));
        QCOMPARE(r.value(JobModel::CancelRequestedRole).toBool(), job.cancelRequested);
        QCOMPARE(r.value(JobModel::ProgressTextRole).toString(), job.progressText);
        QCOMPARE(r.value(JobModel::QueuedTimeRole).toDateTime(), job.queuedAt);
        QCOMPARE(r.value(JobModel::StartedTimeRole).toDateTime(), job.startedAt);
        QCOMPARE(r.value(JobModel::FinishedTimeRole).toDateTime(), job.finishedAt);
        QCOMPARE(r.value(JobModel::ReasonRole).toString(), job.reason);
        QCOMPARE(r.value(JobModel::ResultStatusRole).isValid(), job.resultStatus.has_value());
        if (job.resultStatus.has_value())
            QCOMPARE(r.value(JobModel::ResultStatusRole).toInt(), int(*job.resultStatus));
        QCOMPARE(r.value(JobModel::IsFinishedRole).toBool(), true);
        QVERIFY(job.isFinished());
        QCOMPARE(HistoryView::text(r, JobModel::SessionColumn), job.sessionName);
        QCOMPARE(HistoryView::text(r, JobModel::StateColumn), JobModel::stateText(job.state));
    }
}

void JobModelTest::neverMoreThanOneRunningRow()
{
    setInput("s1", "G_IN", 1);
    setInput("s2", "G_IN", 2);
    setInput("s3", "G_IN", 3);
    request("s1", "gated");
    request("s2", "gated");
    request("s3", "gated");
    for (int i = 0; i < 3; ++i) {
        QVERIFY(gate().waitEntered());
        gate().open(1);
    }
    QVERIFY(waitIdle(*m_queue));

    QCOMPARE(m_view->maxRunningRows(), 1);
    for (const JobId id : {JobId(1), JobId(2), JobId(3)})
        QCOMPARE(m_view->states(id), QList<int>({Q, R, kSucceeded}));
}

void JobModelTest::timestampsAreOrdered()
{
    setInput("s1", "G_IN", 4);
    setInput("s2", "EA_IN", 4);
    setInput("s3", "EA_IN", 4);
    const JobId first = request("s1", "gated");
    const JobId second = request("s2", "expA");
    const JobId neverRan = cancelledJob("s3", "expA");
    QVERIFY(gate().waitEntered());
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));

    for (const JobId id : {first, second}) {
        const JobRecord job = m_queue->job(id);
        QVERIFY(job.queuedAt.isValid() && job.startedAt.isValid() && job.finishedAt.isValid());
        QCOMPARE(job.queuedAt.timeSpec(), Qt::UTC);
        QCOMPARE(job.startedAt.timeSpec(), Qt::UTC);
        QCOMPARE(job.finishedAt.timeSpec(), Qt::UTC);
        QVERIFY(job.queuedAt <= job.startedAt);
        QVERIFY(job.startedAt <= job.finishedAt);
    }
    QVERIFY(m_queue->job(second).startedAt >= m_queue->job(first).finishedAt);

    const JobRecord job = m_queue->job(neverRan);
    QVERIFY(!job.startedAt.isValid());
    QVERIFY(job.queuedAt <= job.finishedAt);
    QCOMPARE(job.finishedAt.timeSpec(), Qt::UTC);
}

void JobModelTest::progressIsItsOwnSignal()
{
    setInput("s1", "G_IN", 4);
    QSignalSpy dataSpy(model(), &QAbstractItemModel::dataChanged);
    QSignalSpy progressSpy(m_queue.get(), &JobQueue::jobProgress);
    const JobId id = request("s1", "gated");
    QVERIFY(gate().waitEntered());
    QTRY_COMPARE(m_queue->job(id).progressText, QStringLiteral("step 1"));

    // Queued -> Running (whole row), then the progress cell alone
    QCOMPARE(dataSpy.count(), 2);
    const QList<QVariant> progress = dataSpy.at(1);
    QCOMPARE(progress.at(0).toModelIndex(), model()->index(0, JobModel::ProgressColumn));
    QCOMPARE(progress.at(1).toModelIndex(), model()->index(0, JobModel::ProgressColumn));
    QCOMPARE(progress.at(2).value<QList<int>>(), QList<int>({Qt::DisplayRole, JobModel::ProgressTextRole}));
    const QList<QVariant> started = dataSpy.at(0);
    QCOMPARE(started.at(0).toModelIndex(), model()->index(0, 0));
    QCOMPARE(started.at(1).toModelIndex(), model()->index(0, JobModel::ColumnCount - 1));
    QVERIFY(started.at(2).value<QList<int>>().contains(JobModel::StateRole));
    QCOMPARE(progressSpy.count(), 1);
    QCOMPARE(progressSpy.at(0).at(0).toULongLong(), qulonglong(id));
    QCOMPARE(progressSpy.at(0).at(1).toString(), QStringLiteral("step 1"));
    QCOMPARE(model()->index(0, JobModel::ProgressColumn).data().toString(), QStringLiteral("step 1"));

    // The last text is recorded before the job ends, and kept
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(dataSpy.count(), 4);       // + "step 2", + the end transition
    QCOMPARE(dataSpy.at(2).at(2).value<QList<int>>(), QList<int>({Qt::DisplayRole, JobModel::ProgressTextRole}));
    QVERIFY(dataSpy.at(3).at(2).value<QList<int>>().contains(JobModel::IsFinishedRole));
    QCOMPARE(m_queue->job(id).progressText, QStringLiteral("step 2"));
    QCOMPARE(m_queue->job(id).state, JobState::Succeeded);
    QCOMPARE(progressSpy.count(), 2);
}

void JobModelTest::cancelRequestedIsVisible()
{
    setInput("s1", "G_IN", 4);
    const JobId id = request("s1", "gated");
    QVERIFY(gate().waitEntered());

    QSignalSpy dataSpy(model(), &QAbstractItemModel::dataChanged);
    QVERIFY(m_queue->cancel(id));
    QVERIFY(dataSpy.count() >= 1);
    const QList<QVariant> asked = dataSpy.at(0);
    QCOMPARE(asked.at(0).toModelIndex(), model()->index(0, 0));
    QCOMPARE(asked.at(1).toModelIndex(), model()->index(0, JobModel::ColumnCount - 1));
    QVERIFY(asked.at(2).value<QList<int>>().contains(JobModel::CancelRequestedRole));
    QCOMPARE(model()->index(0, 0).data(JobModel::CancelRequestedRole), QVariant(true));
    QCOMPARE(model()->index(0, 0).data(JobModel::StateRole), QVariant(int(JobState::Running)));
    QCOMPARE(m_view->row(0).value(JobModel::CancelRequestedRole), QVariant(true));

    // Asking again is not another transition
    const qsizetype signalsSoFar = dataSpy.count();
    QVERIFY(m_queue->cancel(id));
    QCOMPARE(dataSpy.count(), signalsSoFar);

    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(m_view->states(id), QList<int>({Q, R, kCancelled}));
    QCOMPARE(model()->index(0, 0).data(JobModel::CancelRequestedRole), QVariant(false));   // it has stopped
}

void JobModelTest::removeFinishedAndClear()
{
    setInput("s1", "G_IN", 4);
    setInput("s2", "EA_IN", 4);
    setInput("s3", "EA_IN", 4);
    const JobId a = cancelledJob("s2", "expA");
    const JobId active = request("s1", "gated");
    const JobId b = cancelledJob("s2", "expA");
    const JobId c = cancelledJob("s3", "expA");
    const JobId queued = request("s3", "expA");
    const JobId d = cancelledJob("s2", "expA");
    QVERIFY(gate().waitEntered());
    QCOMPARE(model()->rowCount(), 6);

    QSignalSpy removedSpy(model(), &QAbstractItemModel::rowsRemoved);
    QVERIFY(!model()->removeFinished(active));
    QVERIFY(!model()->removeFinished(queued));
    QVERIFY(!model()->removeFinished(JobId(99)));
    QCOMPARE(removedSpy.count(), 0);

    QVERIFY(model()->removeFinished(b));
    QCOMPARE(removedSpy.count(), 1);
    QCOMPARE(removedSpy.at(0).at(1).toInt(), 2);
    QCOMPARE(removedSpy.at(0).at(2).toInt(), 2);
    QVERIFY(!model()->removeFinished(b));
    QCOMPARE(model()->rowOf(b), -1);
    QCOMPARE(model()->rowOf(a), 0);
    QCOMPARE(model()->rowOf(active), 1);
    QCOMPARE(model()->rowOf(c), 2);
    QCOMPARE(model()->rowOf(queued), 3);
    QCOMPARE(model()->rowOf(d), 4);
    QCOMPARE(m_queue->job(b).id, JobId(0));

    // Every finished row goes; the active ones stay, in order
    QCOMPARE(model()->clearFinished(), 3);
    QCOMPARE(model()->rowCount(), 2);
    QCOMPARE(model()->rowOf(active), 0);
    QCOMPARE(model()->rowOf(queued), 1);
    QCOMPARE(model()->clearFinished(), 0);
    QCOMPARE(m_view->rowCount(), 2);
    QCOMPARE(m_view->row(0).value(JobModel::JobIdRole).toULongLong(), qulonglong(active));

    // The queue still finds its jobs after the rows moved
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(m_queue->job(active).state, JobState::Succeeded);
    QCOMPARE(m_queue->job(queued).state, JobState::Succeeded);
    QCOMPARE(model()->clearFinished(), 2);
    QCOMPARE(model()->rowCount(), 0);
}

// removeRows() is what a view's standard deletion calls.
void JobModelTest::removeRowsRefusesActive()
{
    setInput("s1", "G_IN", 4);
    setInput("s2", "EA_IN", 4);
    setInput("s3", "EA_IN", 4);
    const JobId a = cancelledJob("s2", "expA");
    const JobId b = cancelledJob("s2", "expA");
    const JobId active = request("s1", "gated");
    const JobId queued = request("s3", "expA");
    QVERIFY(gate().waitEntered());

    QVERIFY(!model()->removeRows(1, 2));        // finished + running
    QVERIFY(!model()->removeRows(2, 1));        // running
    QVERIFY(!model()->removeRows(3, 1));        // queued
    QVERIFY(!model()->removeRows(-1, 1));
    QVERIFY(!model()->removeRows(0, 0));
    QVERIFY(!model()->removeRows(0, 99));
    QVERIFY(!model()->removeRows(0, 1, model()->index(0, 0)));
    QCOMPARE(model()->rowCount(), 4);

    QVERIFY(model()->removeRows(0, 2));
    QCOMPARE(model()->rowCount(), 2);
    QCOMPARE(model()->rowOf(a), -1);
    QCOMPARE(model()->rowOf(b), -1);
    QCOMPARE(model()->rowOf(active), 0);
    QCOMPARE(model()->rowOf(queued), 1);

    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QVERIFY(model()->removeRow(1));
    QVERIFY(model()->removeRow(0));
    QCOMPARE(model()->rowCount(), 0);
}

void JobModelTest::retentionBound()
{
    QCOMPARE(model()->finishedLimit(), 200);
    model()->setFinishedLimit(3);
    QCOMPARE(model()->finishedLimit(), 3);

    setInput("s1", "G_IN", 4);
    setInput("s2", "EA_IN", 4);
    const JobId active = request("s1", "gated");        // older than every finished job
    QVERIFY(gate().waitEntered());
    const JobId f1 = cancelledJob("s2", "expA");
    const JobId f2 = cancelledJob("s2", "expA");
    const JobId f3 = cancelledJob("s2", "expA");
    QCOMPARE(model()->rowCount(), 4);

    // The fourth finished job: its end transition is signalled first, then
    // the oldest FINISHED row goes, and only that one.
    QStringList order;
    const auto c1 = connect(model(), &QAbstractItemModel::dataChanged, this,
                            [&order](const QModelIndex &topLeft, const QModelIndex &, const QList<int> &roles) {
        if (roles.contains(JobModel::IsFinishedRole))
            order.append(QStringLiteral("finished %1").arg(topLeft.data(JobModel::JobIdRole).toULongLong()));
    });
    const auto c2 = connect(model(), &QAbstractItemModel::rowsRemoved, this,
                            [&order](const QModelIndex &, int first, int last) {
        order.append(QStringLiteral("removed %1-%2").arg(first).arg(last));
    });
    const JobId f4 = cancelledJob("s2", "expA");
    disconnect(c1);
    disconnect(c2);
    QCOMPARE(order, QStringList({QStringLiteral("finished %1").arg(f4), QStringLiteral("removed 1-1")}));
    QCOMPARE(model()->rowCount(), 4);
    QCOMPARE(model()->rowOf(f1), -1);
    QCOMPARE(model()->rowOf(active), 0);
    QCOMPARE(model()->rowOf(f2), 1);
    QCOMPARE(model()->rowOf(f3), 2);
    QCOMPARE(model()->rowOf(f4), 3);
    QCOMPARE(m_queue->job(active).state, JobState::Running);

    // Lowering the limit trims at once; a negative limit is zero
    model()->setFinishedLimit(1);
    QCOMPARE(model()->rowCount(), 2);
    QCOMPARE(model()->rowOf(f4), 1);
    model()->setFinishedLimit(-5);
    QCOMPARE(model()->finishedLimit(), 0);
    QCOMPARE(model()->rowCount(), 1);
    QCOMPARE(model()->rowOf(active), 0);

    // With a limit of zero a job's row goes as soon as it has ended
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(model()->rowCount(), 0);
    QCOMPARE(m_view->states(active), QList<int>({Q, R, kSucceeded}));
    QCOMPARE(m_view->rowCount(), 0);
}

// Spec 8.4: nothing is persisted. Jobs of every ending leave the settings and
// the logbook folder byte-identical.
void JobModelTest::nothingIsPersisted()
{
    TestEnvironment &env = TestEnvironment::instance();
    setInput("s1", "G_IN", 4);
    setInput("s1", "X_IN", 4);
    setInput("s2", "EA_IN", -1);
    setInput("s3", "G_IN", 3);
    QVERIFY(waitForIdle(*m_sessions));
    m_sessions->flushDirtySessions();
    QSettings().sync();

    const QMap<QString, QByteArray> settingsBefore = snapshot(env.settingsPath());
    const QMap<QString, QByteArray> logbookBefore = snapshot(env.logbookDir());
    QVERIFY(!logbookBefore.isEmpty());

    const JobId success = request("s1", "gated");
    const JobId rejection = request("s2", "expA");
    const JobId failure = request("s1", "exhausted");
    const JobId cancelled = request("s3", "gated");
    QVERIFY(gate().waitEntered());
    gate().open(1);
    QTRY_COMPARE(m_queue->job(failure).state, JobState::Failed);
    QVERIFY(gate().waitEntered());
    QVERIFY(m_queue->cancel(cancelled));
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(m_queue->job(success).state, JobState::Succeeded);
    QCOMPARE(m_queue->job(rejection).state, JobState::Succeeded);
    QCOMPARE(m_queue->job(cancelled).state, JobState::Cancelled);
    model()->clearFinished();
    m_queue->shutdown();

    QVERIFY(waitForIdle(*m_sessions));
    m_sessions->flushDirtySessions();
    QSettings().sync();
    QCOMPARE(snapshot(env.settingsPath()), settingsBefore);
    QCOMPARE(snapshot(env.logbookDir()), logbookBefore);
}

FLYSIGHT_TEST_MAIN(JobModelTest)
#include "tst_jobmodel.moc"
