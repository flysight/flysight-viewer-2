#ifndef JOBMODEL_H
#define JOBMODEL_H

#include <optional>

#include <QAbstractTableModel>
#include <QDateTime>
#include <QHash>
#include <QMetaType>
#include <QString>
#include <QVector>

#include "dependencykey.h"
#include "engine/calctypes.h"

namespace FlySight {

class JobQueue;

/// Identifies one job for the life of the application. 0 means "no job"; ids
/// start at 1, ascend in request order, and are never reused.
using JobId = quint64;

/// queued -> running -> exactly one of the four end states (a queued job may
/// also end without ever running). See JobQueue for who decides which.
enum class JobState {
    Queued,
    Running,
    Succeeded,      ///< the result was published (a rejection or a cached failure included)
    Cancelled,      ///< by the user, by shutdown, or because nothing wanted it any more
    Superseded,     ///< the engine refused: inputs changed, session or registration gone
    Failed          ///< the environment prevented completion; nothing was cached
};

/// One of the four end states.
constexpr bool isEndState(JobState state)
{
    return state != JobState::Queued && state != JobState::Running;
}

/// Everything known about one job: a job is one explicit calculation for one
/// session. A plain value; the copy a caller holds never changes.
struct JobRecord {
    JobId          id = 0;
    QString        sessionId;
    QString        sessionName;             ///< snapshot at request: _DESCRIPTION, else the session id
    CalculationId  calculationId;           ///< registration id
    DependencyKey  instanceOutput = DependencyKey::attribute(QString());   ///< empty name: plain calculation
    QString        instanceId;              ///< == calculationId for a plain calculation
    QString        calculationTitle;        ///< CalculationBlocker::title
    JobState       state = JobState::Queued;
    bool           cancelRequested = false; ///< Running only: asked to stop, not stopped yet
    QString        progressText;            ///< latest text; kept after the job ends
    QDateTime      queuedAt;                ///< UTC
    QDateTime      startedAt;               ///< UTC; invalid if compute never started
    QDateTime      finishedAt;              ///< UTC; invalid while the job is active
    QString        reason;                  ///< outcome reason; empty for a plain success
    std::optional<ResultStatus> resultStatus;   ///< Succeeded only: the published status

    /// The state is one of the four end states.
    bool isFinished() const { return isEndState(state); }
    /// Queued or Running.
    bool isActive() const { return !isFinished(); }
};

/// The jobs of the application, current and finished, as a Qt table model.
///
/// The model is the STORE of the job records, not a copy of them: JobQueue
/// keeps no job list of its own, so what a view sees is, by construction, what
/// the queue acts on. Only JobQueue changes job state (it is a friend, for the
/// private mutators and nothing else: it reads through records() like anyone);
/// anyone may remove FINISHED rows. Main thread only. A pure container: it never
/// reads the clock, the sessions, or the settings, and nothing is persisted.
///
/// ROWS. One row per job, in request order (ascending JobId). New rows are
/// appended; a job keeps its row for life, so a row index changes only when
/// earlier rows are removed. Use JobIdRole, not the row, to remember a job.
///
/// COLUMNS (Qt::DisplayRole): session name; calculation title; stateText();
/// progress text; queued, started, finished as local-time text (empty when
/// invalid); reason. Every custom role is answered on every column. Time roles
/// return QDateTime (UTC), StateRole int(JobState), ResultStatusRole
/// int(ResultStatus) or an invalid QVariant.
///
/// SIGNALS. Every transition is its own signal, never coalesced:
///   rowsInserted                                   a job was queued
///   dataChanged(row, 0 .. ColumnCount-1)           Queued -> Running; cancel requested; the end transition
///   dataChanged(row, ProgressColumn),
///       roles {Qt::DisplayRole, ProgressTextRole}  progress text
///   rowsRemoved                                    removal and retention trimming
/// No modelReset is ever emitted after construction.
///
/// REMOVAL. removeFinished(), clearFinished() and removeRows() remove FINISHED
/// rows only. removeRows() over a range that contains an active job removes
/// nothing and returns false.
///
/// RETENTION. Finished rows are kept for the life of the application up to
/// finishedLimit() (default 200). When a job finishes and the count exceeds
/// the limit, the oldest finished rows are removed after the end transition
/// has been signalled. Active rows are never trimmed.
class JobModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column {
        SessionColumn,
        CalculationColumn,
        StateColumn,
        ProgressColumn,
        QueuedColumn,
        StartedColumn,
        FinishedColumn,
        ReasonColumn,
        ColumnCount
    };

    enum Roles {
        JobIdRole = Qt::UserRole + 1,   ///< qulonglong
        SessionIdRole,
        SessionNameRole,
        CalculationIdRole,
        InstanceIdRole,
        CalculationTitleRole,
        StateRole,                      ///< int(JobState)
        CancelRequestedRole,            ///< bool
        ProgressTextRole,
        QueuedTimeRole,                 ///< QDateTime, UTC
        StartedTimeRole,                ///< QDateTime, UTC; invalid until the job runs
        FinishedTimeRole,               ///< QDateTime, UTC; invalid until the job ends
        ReasonRole,
        ResultStatusRole,               ///< int(ResultStatus), or invalid
        IsFinishedRole                  ///< bool
    };

    static constexpr int kDefaultFinishedLimit = 200;

    explicit JobModel(QObject *parent = nullptr);

    // QAbstractTableModel
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QHash<int, QByteArray> roleNames() const override;
    /// Finished rows only; false (nothing removed) when the range is invalid or
    /// contains an active job.
    bool removeRows(int row, int count, const QModelIndex &parent = QModelIndex()) override;

    int rowOf(JobId id) const;              ///< -1 when unknown (or already removed)
    JobRecord record(int row) const;        ///< by value; a default record (id 0) when out of range
    JobRecord record(JobId id) const;       ///< a default record (id 0) when unknown
    /// Every record, in row order. The reference is valid until the next
    /// change of the model: do not hold it across anything that can emit.
    const QVector<JobRecord> &records() const { return m_jobs; }
    static QString stateText(JobState state);   ///< tr(): "Queued", "Running", ...

    bool removeFinished(JobId id);          ///< false for an active or unknown job
    int  clearFinished();                   ///< number removed
    int  finishedLimit() const { return m_finishedLimit; }
    void setFinishedLimit(int limit);       ///< >= 0; trims at once, oldest finished first

private:
    // The mutators of JobQueue, and the only members it uses as a friend. Each
    // emits exactly the signals listed under SIGNALS. Times come from the
    // queue: the model never reads the clock.
    friend class JobQueue;      // append() ... trimFinished()
    JobId append(const JobRecord &record);      // the queue assigns the id; returns it
    void markRunning(JobId id, const QDateTime &startedAt);
    void markCancelRequested(JobId id);
    void setProgress(JobId id, const QString &text);
    /// The one place a job ends: asserts that the job is active and `state` an
    /// end state. A second end for the same job is ignored (and asserts).
    void markFinished(JobId id, JobState state, const QString &reason,
                      std::optional<ResultStatus> resultStatus, const QDateTime &finishedAt);
    void trimFinished();                        // retention; oldest finished first

    void removeRun(int first, int last);        // rows [first, last], all finished
    void rebuildIndex();
    void emitRowChanged(int row, const QList<int> &roles);

    QVector<JobRecord> m_jobs;                  // request order
    QHash<JobId, int> m_rowById;
    int m_finishedLimit = kDefaultFinishedLimit;
};

} // namespace FlySight

Q_DECLARE_METATYPE(FlySight::JobState)

#endif // JOBMODEL_H
