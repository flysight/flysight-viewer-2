#include "jobmodel.h"

#include <QLocale>

namespace FlySight {

namespace {

QString timeText(const QDateTime &utc)
{
    if (!utc.isValid())
        return QString();
    return QLocale().toString(utc.toLocalTime(), QLocale::ShortFormat);
}

} // namespace

JobModel::JobModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

int JobModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_jobs.size());
}

int JobModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(ColumnCount);
}

QVariant JobModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.parent().isValid()
        || index.row() < 0 || index.row() >= m_jobs.size()
        || index.column() < 0 || index.column() >= ColumnCount)
        return QVariant();

    const JobRecord &job = m_jobs.at(index.row());

    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case SessionColumn:     return job.sessionName;
        case CalculationColumn: return job.calculationTitle;
        case StateColumn:       return stateText(job.state);
        case ProgressColumn:    return job.progressText;
        case QueuedColumn:      return timeText(job.queuedAt);
        case StartedColumn:     return timeText(job.startedAt);
        case FinishedColumn:    return timeText(job.finishedAt);
        case ReasonColumn:      return job.reason;
        }
        return QVariant();

    // The custom roles describe the job, not the cell: same answer on every column
    case JobIdRole:             return QVariant::fromValue<qulonglong>(job.id);
    case SessionIdRole:         return job.sessionId;
    case SessionNameRole:       return job.sessionName;
    case CalculationIdRole:     return job.calculationId;
    case InstanceIdRole:        return job.instanceId;
    case CalculationTitleRole:  return job.calculationTitle;
    case StateRole:             return int(job.state);
    case CancelRequestedRole:   return job.cancelRequested;
    case ProgressTextRole:      return job.progressText;
    case QueuedTimeRole:        return job.queuedAt;
    case StartedTimeRole:       return job.startedAt;
    case FinishedTimeRole:      return job.finishedAt;
    case ReasonRole:            return job.reason;
    case ResultStatusRole:
        return job.resultStatus.has_value() ? QVariant(int(*job.resultStatus)) : QVariant();
    case IsFinishedRole:        return job.isFinished();
    }
    return QVariant();
}

QVariant JobModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QVariant();

    switch (section) {
    case SessionColumn:     return tr("Session");
    case CalculationColumn: return tr("Calculation");
    case StateColumn:       return tr("State");
    case ProgressColumn:    return tr("Progress");
    case QueuedColumn:      return tr("Queued");
    case StartedColumn:     return tr("Started");
    case FinishedColumn:    return tr("Finished");
    case ReasonColumn:      return tr("Reason");
    }
    return QVariant();
}

Qt::ItemFlags JobModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

QHash<int, QByteArray> JobModel::roleNames() const
{
    QHash<int, QByteArray> roles = QAbstractTableModel::roleNames();
    roles[JobIdRole]            = "jobId";
    roles[SessionIdRole]        = "sessionId";
    roles[SessionNameRole]      = "sessionName";
    roles[CalculationIdRole]    = "calculationId";
    roles[InstanceIdRole]       = "instanceId";
    roles[CalculationTitleRole] = "calculationTitle";
    roles[StateRole]            = "state";
    roles[CancelRequestedRole]  = "cancelRequested";
    roles[ProgressTextRole]     = "progressText";
    roles[QueuedTimeRole]       = "queuedTime";
    roles[StartedTimeRole]      = "startedTime";
    roles[FinishedTimeRole]     = "finishedTime";
    roles[ReasonRole]           = "reason";
    roles[ResultStatusRole]     = "resultStatus";
    roles[IsFinishedRole]       = "isFinished";
    return roles;
}

bool JobModel::removeRows(int row, int count, const QModelIndex &parent)
{
    if (parent.isValid() || row < 0 || count <= 0 || row + count > m_jobs.size())
        return false;

    // All or nothing: an active job is never removed, so neither is a range
    // that contains one.
    for (int i = row; i < row + count; ++i) {
        if (m_jobs.at(i).isActive())
            return false;
    }

    removeRun(row, row + count - 1);
    return true;
}

int JobModel::rowOf(JobId id) const
{
    return m_rowById.value(id, -1);
}

JobRecord JobModel::record(int row) const
{
    if (row < 0 || row >= m_jobs.size())
        return JobRecord();
    return m_jobs.at(row);
}

JobRecord JobModel::record(JobId id) const
{
    return record(rowOf(id));
}

QString JobModel::stateText(JobState state)
{
    switch (state) {
    case JobState::Queued:      return tr("Queued");
    case JobState::Running:     return tr("Running");
    case JobState::Succeeded:   return tr("Succeeded");
    case JobState::Cancelled:   return tr("Cancelled");
    case JobState::Superseded:  return tr("Superseded");
    case JobState::Failed:      return tr("Failed");
    }
    return QString();
}

bool JobModel::removeFinished(JobId id)
{
    const int row = rowOf(id);
    if (row < 0 || m_jobs.at(row).isActive())
        return false;
    removeRun(row, row);
    return true;
}

int JobModel::clearFinished()
{
    // Back to front, one contiguous run of finished rows per rowsRemoved
    int removed = 0;
    int last = int(m_jobs.size()) - 1;
    while (last >= 0) {
        if (m_jobs.at(last).isActive()) {
            --last;
            continue;
        }
        int first = last;
        while (first > 0 && m_jobs.at(first - 1).isFinished())
            --first;
        removeRun(first, last);
        removed += last - first + 1;
        last = first - 1;
    }
    return removed;
}

void JobModel::setFinishedLimit(int limit)
{
    m_finishedLimit = qMax(0, limit);
    trimFinished();
}

// ---- Mutators (JobQueue only) -------------------------------------------

JobId JobModel::append(const JobRecord &record)
{
    Q_ASSERT(record.id != 0 && !m_rowById.contains(record.id));
    Q_ASSERT(m_jobs.isEmpty() || m_jobs.last().id < record.id);    // request order

    const int row = int(m_jobs.size());
    beginInsertRows(QModelIndex(), row, row);
    m_jobs.append(record);
    m_rowById.insert(record.id, row);
    endInsertRows();
    return record.id;
}

void JobModel::markRunning(JobId id, const QDateTime &startedAt)
{
    const int row = rowOf(id);
    Q_ASSERT(row >= 0 && m_jobs.at(row).state == JobState::Queued);
    if (row < 0 || m_jobs.at(row).state != JobState::Queued)
        return;

    JobRecord &job = m_jobs[row];
    job.state = JobState::Running;
    job.startedAt = startedAt;
    job.progressText.clear();
    emitRowChanged(row, {Qt::DisplayRole, StateRole, StartedTimeRole, ProgressTextRole});
}

void JobModel::markCancelRequested(JobId id)
{
    const int row = rowOf(id);
    Q_ASSERT(row >= 0 && m_jobs.at(row).state == JobState::Running);
    if (row < 0 || m_jobs.at(row).state != JobState::Running || m_jobs.at(row).cancelRequested)
        return;

    m_jobs[row].cancelRequested = true;
    emitRowChanged(row, {CancelRequestedRole});
}

void JobModel::setProgress(JobId id, const QString &text)
{
    const int row = rowOf(id);
    if (row < 0 || m_jobs.at(row).state != JobState::Running)
        return;

    m_jobs[row].progressText = text;
    const QModelIndex cell = index(row, ProgressColumn);
    emit dataChanged(cell, cell, {Qt::DisplayRole, ProgressTextRole});
}

void JobModel::markFinished(JobId id, JobState state, const QString &reason,
                            std::optional<ResultStatus> resultStatus, const QDateTime &finishedAt)
{
    const int row = rowOf(id);
    Q_ASSERT_X(isEndState(state), "JobModel::markFinished", "not an end state");
    Q_ASSERT_X(row >= 0 && m_jobs.at(row).isActive(), "JobModel::markFinished",
               "a job ends exactly once");
    if (!isEndState(state) || row < 0 || m_jobs.at(row).isFinished())
        return;

    JobRecord &job = m_jobs[row];
    job.state = state;
    job.cancelRequested = false;    // it has stopped
    job.reason = reason;
    job.resultStatus = state == JobState::Succeeded ? resultStatus : std::nullopt;
    job.finishedAt = finishedAt;
    emitRowChanged(row, {Qt::DisplayRole, StateRole, CancelRequestedRole, FinishedTimeRole,
                         ReasonRole, ResultStatusRole, IsFinishedRole});
}

void JobModel::trimFinished()
{
    int finished = 0;
    for (const JobRecord &job : std::as_const(m_jobs)) {
        if (job.isFinished())
            ++finished;
    }

    // Oldest finished first; active rows are stepped over
    int row = 0;
    while (finished > m_finishedLimit && row < m_jobs.size()) {
        if (m_jobs.at(row).isActive()) {
            ++row;
            continue;
        }
        removeRun(row, row);
        --finished;
    }
}

void JobModel::removeRun(int first, int last)
{
    beginRemoveRows(QModelIndex(), first, last);
    m_jobs.remove(first, last - first + 1);
    rebuildIndex();
    endRemoveRows();
}

void JobModel::rebuildIndex()
{
    m_rowById.clear();
    m_rowById.reserve(m_jobs.size());
    for (int i = 0; i < m_jobs.size(); ++i)
        m_rowById.insert(m_jobs.at(i).id, i);
}

void JobModel::emitRowChanged(int row, const QList<int> &roles)
{
    emit dataChanged(index(row, 0), index(row, ColumnCount - 1), roles);
}

} // namespace FlySight
