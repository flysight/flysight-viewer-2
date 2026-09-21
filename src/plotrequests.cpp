#include "plotrequests.h"

#include <algorithm>

#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "jobqueue.h"
#include "plotmodel.h"
#include "sessiondata.h"
#include "sessionmodel.h"

namespace FlySight {

namespace {

DependencyKey yName(const PlotValue &plot)
{
    return DependencyKey::measurement(plot.sensorID, plot.measurementID);
}

QString titleOf(const CalculationBlocker &calculation)
{
    return calculation.title.isEmpty() ? calculation.instanceId : calculation.title;
}

// What the queue's answer means for the track that asked
bool gotJob(const JobQueue::RequestResult &result)
{
    using Kind = JobQueue::RequestResult::Kind;
    return result.kind == Kind::Created || result.kind == Kind::AlreadyActive;
}

// "There will never be a job for this": Blocked, SessionNotLoaded and
// ShuttingDown are not refusals - the track stays as inspection reports it.
bool wasRefused(const JobQueue::RequestResult &result)
{
    using Kind = JobQueue::RequestResult::Kind;
    return result.kind == Kind::MissingInput || result.kind == Kind::NothingToDo
        || result.kind == Kind::UnknownCalculation;
}

} // namespace

// ---- Construction ---------------------------------------------------------------

PlotRequests::PlotRequests(SessionModel *sessionModel, PlotModel *plotModel, JobQueue *jobQueue,
                           QObject *parent)
    : QObject(parent)
    , m_sessionModel(sessionModel)
    , m_plotModel(plotModel)
    , m_jobQueue(jobQueue)
{
    m_updateTimer.setSingleShot(true);
    m_updateTimer.setInterval(0);
    connect(&m_updateTimer, &QTimer::timeout, this, &PlotRequests::recompute);

    if (m_jobQueue) {
        connect(m_jobQueue, &JobQueue::jobsChanged, this, &PlotRequests::scheduleUpdate);
        connect(m_jobQueue, &JobQueue::jobFinished, this, &PlotRequests::onJobFinished);
        connect(m_jobQueue, &JobQueue::jobCancelRequested, this, &PlotRequests::onJobCancelRequested);
        connect(m_jobQueue, &JobQueue::jobProgress, this, &PlotRequests::onJobProgress);
    }

    if (m_sessionModel) {
        connect(m_sessionModel, &SessionModel::dependencyChanged, this, &PlotRequests::onDependencyChanged);
        connect(m_sessionModel, &SessionModel::visibilityChanged, this, &PlotRequests::onVisibilityChanged);
        connect(m_sessionModel, &SessionModel::modelChanged, this, &PlotRequests::scheduleUpdate);
        // The background loader announces visibility only at the end of a
        // batch; a track becomes one the moment its session is loaded
        connect(m_sessionModel, &SessionModel::sessionLoaded, this,
                [this](const QString &) { scheduleUpdate(); });
        connect(m_sessionModel, &QAbstractItemModel::modelReset, this, &PlotRequests::onSessionModelReset);
    }

    if (m_plotModel) {
        connect(m_plotModel, &QAbstractItemModel::dataChanged, this, &PlotRequests::onPlotDataChanged);
        connect(m_plotModel, &QAbstractItemModel::modelReset, this, &PlotRequests::onPlotCheckStateChanged);
    }

    // The observer only clears memos and starts the timer: it runs
    // synchronously inside the registration, possibly during plugin loading
    m_registryObserver = CalculationRegistry::instance().addObserver([this] { onRegistryChanged(); });

    // Rows restored at startup show their refresh control without any event
    syncCheckedSet();
    scheduleUpdate();
}

PlotRequests::~PlotRequests()
{
    CalculationRegistry::instance().removeObserver(m_registryObserver);
}

QString PlotRequests::plotId(const QString &sensorId, const QString &measurementId)
{
    return sensorId + QLatin1Char('/') + measurementId;
}

QString PlotRequests::plotId(const PlotValue &plot)
{
    return plotId(plot.sensorID, plot.measurementID);
}

PlotRowState PlotRequests::rowState(const QString &plotId) const
{
    return m_states.value(plotId);
}

bool PlotRequests::isInert() const
{
    return !m_sessionModel || !m_plotModel || !m_jobQueue;
}

// ---- Which plots matter -----------------------------------------------------------

// The registry is the one authority (CalculationRegistry::dependsOnExplicit());
// the memo here is per plot id and also keeps the static names of the plot.
bool PlotRequests::isExplicitBacked(const PlotValue &plot)
{
    const QString id = plotId(plot);
    const auto known = m_explicitBacked.constFind(id);
    if (known != m_explicitBacked.constEnd())
        return known.value();

    const CalculationRegistry &registry = CalculationRegistry::instance();
    const QSet<DependencyKey> names = registry.staticDependencies(yName(plot)).names;
    const bool backed = registry.dependsOnExplicit(yName(plot));

    m_explicitBacked.insert(id, backed);
    m_staticNames.insert(id, names);
    return backed;
}

// Brings m_checked in line with the PlotModel. A plot that was unchecked, or
// that vanished in a reset, loses its waiting set here, at once.
bool PlotRequests::syncCheckedSet()
{
    QHash<QString, PlotValue> checked;
    QStringList order;
    if (m_plotModel) {
        const QVector<PlotValue> enabled = m_plotModel->enabledPlots();
        for (const PlotValue &plot : enabled) {
            const QString id = plotId(plot);
            checked.insert(id, plot);
            order.append(id);
        }
    }

    bool anyUnchecked = false;
    for (auto it = m_checked.constBegin(); it != m_checked.constEnd(); ++it) {
        if (!checked.contains(it.key())) {
            anyUnchecked = true;
            m_waiting.remove(it.key());
        }
    }

    if (order != m_checkedOrder)
        m_relevantNamesDirty = true;
    m_checked = checked;
    m_checkedOrder = order;
    return anyUnchecked;
}

void PlotRequests::rebuildRelevantNames()
{
    m_relevantNames.clear();
    for (const QString &id : std::as_const(m_checkedOrder)) {
        if (isExplicitBacked(m_checked.value(id)))
            m_relevantNames.unite(m_staticNames.value(id));
    }
    m_relevantNamesDirty = false;
}

QVector<PlotValue> PlotRequests::inspectedPlots()
{
    QVector<PlotValue> plots;
    for (const QString &id : std::as_const(m_checkedOrder)) {
        const PlotValue plot = m_checked.value(id);
        if (isExplicitBacked(plot))
            plots.append(plot);
    }
    return plots;
}

// ---- Inspection and classification ---------------------------------------------------

// Taken from the queue, not from activeJob(), so that both rules are decided in
// one visible place: a running job that was asked to cancel is not live, and
// when such a job and a newer queued one exist for one key, the queued one is.
PlotRequests::LiveJobs PlotRequests::liveJobs() const
{
    LiveJobs live;
    if (!m_jobQueue)
        return live;
    const QList<JobId> ids = m_jobQueue->activeJobs();
    for (const JobId id : ids) {
        const JobRecord record = m_jobQueue->job(id);
        if (!record.isActive() || (record.state == JobState::Running && record.cancelRequested))
            continue;
        const JobKey key(record.sessionId, record.instanceId);
        if (!live.contains(key))        // request order: the oldest live job wins
            live.insert(key, record);
    }
    return live;
}

BlockerReport PlotRequests::inspectLocked(const SessionData &session, const PlotValue &plot) const
{
    return session.calculationEngine().blockers(yName(plot));
}

// The one guarded read of the component. The tracks are the rows the plot
// widget draws: loaded, visible, and not a failed-load placeholder, in row
// order. Everything returned is a plain value, so that nothing is requested,
// cancelled, or emitted while the guard is held. blockers() may compute
// on-demand values; that reads, it neither loads nor evicts.
PlotRequests::Inspections PlotRequests::inspect(const QVector<PlotValue> &plots,
                                                const QString &onlySessionId) const
{
    Inspections result;
    if (!m_sessionModel || plots.isEmpty())
        return result;

    const SessionModel &model = *m_sessionModel;
    const SessionModel::RowStabilityGuard guard(model);
    const int rows = model.rowCount();
    for (int row = 0; row < rows; ++row) {
        const SessionRow &sr = model.rowAt(row);
        if (!sr.isLoaded() || !sr.visible || sr.loadFailed)
            continue;
        if (!onlySessionId.isEmpty() && sr.sessionId != onlySessionId)
            continue;

        const SessionData &session = sr.session.value();
        Track track;
        track.sessionId = sr.sessionId;
        track.sessionName = session.getAttribute(SessionKeys::Description).toString();
        if (track.sessionName.isEmpty())
            track.sessionName = sr.sessionId;

        for (const PlotValue &plot : plots)
            result[plotId(plot)].append(Inspection{track, inspectLocked(session, plot)});
    }
    return result;
}

bool PlotRequests::hasLiveJob(const QString &sessionId, const BlockerReport &report, const LiveJobs &live)
{
    for (const CalculationBlocker &blocker : report.blockers) {
        if (live.contains(JobKey(sessionId, blocker.instanceId)))
            return true;
    }
    return false;
}

QString PlotRequests::failureReason(const QList<UnproducedNote> &notes)
{
    QStringList parts;
    for (const UnproducedNote &note : notes) {
        QString detail = note.detail;
        if (detail.isEmpty()) {
            detail = note.status == ResultStatus::Failed ? tr("Calculation failed")
                                                         : tr("No result for this recording");
        }
        parts.append(QStringLiteral("%1: %2").arg(titleOf(note.calculation), detail));
    }
    // A failed track always says why
    if (parts.isEmpty())
        parts.append(tr("No result for this recording"));
    return parts.join(QStringLiteral("; "));
}

PlotTrackState PlotRequests::classify(const Track &track, const BlockerReport &report,
                                      const LiveJobs &live) const
{
    PlotTrackState state;
    state.sessionId = track.sessionId;
    state.sessionName = track.sessionName;

    switch (report.state) {
    case BlockerReport::State::Available:
        state.condition = PlotTrackCondition::Available;
        return state;
    case BlockerReport::State::NotApplicable:
        state.condition = PlotTrackCondition::NotApplicable;
        return state;
    case BlockerReport::State::NotProduced:
        state.condition = PlotTrackCondition::Failed;
        for (const UnproducedNote &note : report.notProduced)
            state.calculationTitles.append(titleOf(note.calculation));
        state.reason = failureReason(report.notProduced);
        return state;
    case BlockerReport::State::Blocked:
        break;      // notes on a Blocked report are ignored: Blocked wins
    }

    // The job the track waits on: the running live job among its blockers,
    // else the oldest queued one
    JobRecord job;
    bool allRefused = true;
    for (const CalculationBlocker &blocker : report.blockers) {
        state.calculationTitles.append(titleOf(blocker));
        const JobKey key(track.sessionId, blocker.instanceId);
        if (!m_refused.contains(key))
            allRefused = false;
        const auto found = live.constFind(key);
        if (found == live.constEnd())
            continue;
        const bool better = job.id == 0
            || (job.state != JobState::Running
                && (found->state == JobState::Running || found->id < job.id));
        if (better)
            job = found.value();
    }

    if (job.id != 0) {
        state.condition = PlotTrackCondition::Pending;
        state.job = job.id;
        state.jobState = job.state;
        if (job.state == JobState::Running)
            state.jobProgressText = job.progressText;
    } else if (allRefused) {
        // The queue said there is nothing to run for any of them: a refresh
        // control could never do anything
        state.condition = PlotTrackCondition::NotApplicable;
        state.calculationTitles.clear();
    } else {
        state.condition = PlotTrackCondition::Missing;
    }
    return state;
}

// ---- The pass -------------------------------------------------------------------------

void PlotRequests::scheduleUpdate()
{
    if (m_updateTimer.isActive())
        return;
    m_updateTimer.start();
}

void PlotRequests::flush()
{
    if (m_updateTimer.isActive())
        recompute();
}

bool PlotRequests::hasPendingUpdate() const
{
    return m_updateTimer.isActive();
}

QString PlotRequests::buildToolTip(const PlotRowState &state)
{
    const QString indent = QStringLiteral("  ");
    QStringList lines;

    if (!state.pending.isEmpty()) {
        lines.append(tr("Computing (%1 of %2 done):").arg(state.waitingDone).arg(state.waitingTotal));
        for (const PlotTrackState &track : state.pending) {
            QString progress = tr("queued");
            if (track.jobState == JobState::Running)
                progress = track.jobProgressText.isEmpty() ? tr("running") : track.jobProgressText;
            lines.append(indent + tr("%1 - %2: %3").arg(track.sessionName,
                                                         track.calculationTitles.join(QStringLiteral(", ")),
                                                         progress));
        }
    }
    if (!state.missing.isEmpty()) {
        lines.append(tr("Not computed (press refresh to compute):"));
        for (const PlotTrackState &track : state.missing)
            lines.append(indent + track.sessionName);
    }
    if (!state.failed.isEmpty()) {
        lines.append(tr("Could not be computed:"));
        for (const PlotTrackState &track : state.failed)
            lines.append(indent + tr("%1 - %2").arg(track.sessionName, track.reason));
    }
    return lines.join(QLatin1Char('\n'));
}

// Aggregates one inspected plot and reconciles its waiting set with what the
// tracks are now. `tracks` are the visible loaded tracks in row order.
PlotRowState PlotRequests::buildRowState(const QString &plotId, const QList<PlotTrackState> &tracks)
{
    PlotRowState state;
    state.plotId = plotId;
    state.explicitBacked = true;

    QHash<QString, PlotTrackCondition> conditions;
    for (const PlotTrackState &track : tracks) {
        conditions.insert(track.sessionId, track.condition);
        switch (track.condition) {
        case PlotTrackCondition::Pending: state.pending.append(track); break;
        case PlotTrackCondition::Missing: state.missing.append(track); break;
        case PlotTrackCondition::Failed:  state.failed.append(track); break;
        case PlotTrackCondition::Available:
        case PlotTrackCondition::NotApplicable:
            break;      // silently absent
        }
    }
    state.pendingCount = int(state.pending.size());
    state.missingCount = int(state.missing.size());
    state.failedCount = int(state.failed.size());

    if (state.pendingCount == 0) {
        // The episode is over; the next gesture starts a new denominator
        m_waiting.remove(plotId);
    } else {
        QHash<QString, bool> &waiting = m_waiting[plotId];
        for (auto it = waiting.begin(); it != waiting.end();) {
            if (conditions.contains(it.key()))
                ++it;
            else
                it = waiting.erase(it);     // no longer a visible, loaded track
        }
        // A row that did not ask (checked programmatically while another row's
        // jobs run) waits on them too, but continues nothing
        for (const PlotTrackState &track : std::as_const(state.pending)) {
            if (!waiting.contains(track.sessionId))
                waiting.insert(track.sessionId, false);
        }

        state.waitingTotal = int(waiting.size());
        for (auto it = waiting.constBegin(); it != waiting.constEnd(); ++it) {
            const PlotTrackCondition condition = conditions.value(it.key());
            // A waited-for track that fell back to Missing is not done
            if (condition == PlotTrackCondition::Available || condition == PlotTrackCondition::Failed
                || condition == PlotTrackCondition::NotApplicable)
                ++state.waitingDone;
        }
        state.progressLabel = tr("%1 of %2").arg(state.waitingDone).arg(state.waitingTotal);

        for (const PlotTrackState &track : std::as_const(state.pending)) {
            if (track.jobState == JobState::Running) {
                state.jobProgressText = track.jobProgressText;
                break;      // one job runs at a time
            }
        }
    }

    state.toolTip = buildToolTip(state);
    return state;
}

// Stores the new states and announces the differences. `order` lists the
// inspected plots; a plot that is no longer inspected falls back to the default
// state and is announced once.
void PlotRequests::applyStates(const QStringList &order, const QHash<QString, PlotRowState> &states)
{
    QStringList changed;
    for (const QString &id : order) {
        if (m_states.value(id) != states.value(id))
            changed.append(id);
    }
    QStringList dropped;
    for (auto it = m_states.constBegin(); it != m_states.constEnd(); ++it) {
        if (!states.contains(it.key()))
            dropped.append(it.key());
    }
    std::sort(dropped.begin(), dropped.end());
    changed.append(dropped);

    // Stored before anything is emitted: a slot reads rowState()
    m_states = states;

    for (const QString &id : std::as_const(changed))
        emit rowStateChanged(id);
    if (!changed.isEmpty())
        emit rowStatesChanged();
}

// Nothing here starts work. Classifications are not cached across passes: the
// engine's own caches make a repeated blockers() cheap, and a cache keyed on
// dependencyChanged would be wrong (after A publishes, the blocker of B's
// output changes from A to B although B's output may not be re-announced).
void PlotRequests::recompute()
{
    m_updateTimer.stop();
    ++m_passCount;

    if (!isInert())
        syncCheckedSet();
    const QVector<PlotValue> plots = isInert() ? QVector<PlotValue>() : inspectedPlots();
    if (plots.isEmpty()) {
        // Ordinary plots cost nothing: sessions are not even enumerated
        m_waiting.clear();
        applyStates({}, {});
        return;
    }

    const LiveJobs live = liveJobs();
    const Inspections inspections = inspect(plots);

    QStringList order;
    QHash<QString, PlotRowState> states;
    for (const PlotValue &plot : plots) {
        const QString id = plotId(plot);
        QList<PlotTrackState> tracks;
        const QList<Inspection> inspected = inspections.value(id);
        for (const Inspection &inspection : inspected)
            tracks.append(classify(inspection.track, inspection.report, live));
        order.append(id);
        states.insert(id, buildRowState(id, tracks));
    }

    for (auto it = m_waiting.begin(); it != m_waiting.end();) {
        if (states.contains(it.key()))
            ++it;
        else
            it = m_waiting.erase(it);
    }

    applyStates(order, states);
}

// ---- What starts work ---------------------------------------------------------------------

QList<CalculationBlocker> PlotRequests::requestable(const QString &sessionId, const BlockerReport &report,
                                                    const LiveJobs &live) const
{
    QList<CalculationBlocker> result;
    for (const CalculationBlocker &blocker : report.blockers) {
        const JobKey key(sessionId, blocker.instanceId);
        if (!live.contains(key) && !m_refused.contains(key))
            result.append(blocker);
    }
    return result;
}

void PlotRequests::markRefused(const QString &sessionId, const CalculationBlocker &blocker)
{
    m_refused.insert(JobKey(sessionId, blocker.instanceId));
}

int PlotRequests::plotCheckedByUser(const QString &plotId)
{
    return requestMissing(plotId);
}

int PlotRequests::refreshPressed(const QString &plotId)
{
    return requestMissing(plotId);
}

// The gesture: a one-shot request for the blockers of the plot's missing
// tracks. Nothing is remembered that could request again later, except the
// `continues` flag of the tracks that got a job.
int PlotRequests::requestMissing(const QString &plotId)
{
    if (isInert())
        return 0;

    // dataChanged normally arrived before the view's call; if it did not, this
    // degrades to "nothing requested", never to a request for an unchecked plot
    if (syncCheckedSet())
        pruneUnwantedQueued();
    const auto checked = m_checked.constFind(plotId);
    if (checked == m_checked.constEnd())
        return 0;
    const PlotValue plot = checked.value();
    if (!m_plotModel->isPlotEnabled(plot.sensorID, plot.measurementID) || !isExplicitBacked(plot))
        return 0;

    const LiveJobs live = liveJobs();
    const QList<Inspection> inspected = inspect({plot}).value(plotId);
    // The guard is gone: from here on the queue may be called

    // No pending track means the last episode is over, even if the pass that
    // would have noticed has not run yet
    const bool episodeOpen = std::any_of(inspected.cbegin(), inspected.cend(), [&](const Inspection &i) {
        return i.report.state == BlockerReport::State::Blocked && hasLiveJob(i.track.sessionId, i.report, live);
    });
    if (!episodeOpen)
        m_waiting.remove(plotId);

    int created = 0;
    QStringList waitedFor;
    for (const Inspection &inspection : inspected) {
        if (inspection.report.state != BlockerReport::State::Blocked)
            continue;
        const QString &sessionId = inspection.track.sessionId;

        // An already pending track is adopted, so that a plot checked while
        // another row's job runs still continues its own chain
        bool hasJob = hasLiveJob(sessionId, inspection.report, live);
        const QList<CalculationBlocker> blockers = requestable(sessionId, inspection.report, live);
        for (const CalculationBlocker &blocker : blockers) {
            if (!m_jobQueue)
                break;
            const JobQueue::RequestResult result = m_jobQueue->request(sessionId, blocker);
            if (result.created())
                ++created;
            if (gotJob(result))
                hasJob = true;
            else if (wasRefused(result))
                markRefused(sessionId, blocker);
        }
        if (hasJob)
            waitedFor.append(sessionId);
    }

    if (!waitedFor.isEmpty()) {
        QHash<QString, bool> &waiting = m_waiting[plotId];
        for (const QString &sessionId : std::as_const(waitedFor))
            waiting.insert(sessionId, true);
    }

    // Synchronously: the view sees "pending" before the call returns
    recompute();
    return created;
}

// Chained continuation. Runs inside JobQueue::endJob(), between jobFinished and
// the idle check, so the queue never reports idle between the links of a chain.
// The engine state is current: the publication's dependencyChanged came first.
// Must not cancel, shut down, or spin an event loop.
void PlotRequests::continueAfter(const JobRecord &job)
{
    if (isInert())
        return;
    const QString sessionId = job.sessionId;

    QVector<PlotValue> plots;
    for (const QString &id : std::as_const(m_checkedOrder)) {
        if (!m_waiting.value(id).value(sessionId, false))
            continue;       // no gesture asked about this track for this plot
        const PlotValue plot = m_checked.value(id);
        if (m_plotModel->isPlotEnabled(plot.sensorID, plot.measurementID) && isExplicitBacked(plot))
            plots.append(plot);
    }
    if (plots.isEmpty())
        return;

    const LiveJobs live = liveJobs();
    const Inspections inspections = inspect(plots, sessionId);

    for (const PlotValue &plot : std::as_const(plots)) {
        const QString id = plotId(plot);
        const QList<Inspection> inspected = inspections.value(id);
        if (inspected.isEmpty()) {
            // Not a visible, loaded track any more
            const auto waiting = m_waiting.find(id);
            if (waiting != m_waiting.end())
                waiting->remove(sessionId);
            continue;
        }

        const BlockerReport &report = inspected.first().report;
        bool hasJob = false;
        if (report.state == BlockerReport::State::Blocked) {
            hasJob = hasLiveJob(sessionId, report, live);
            const QList<CalculationBlocker> blockers = requestable(sessionId, report, live);
            for (const CalculationBlocker &blocker : blockers) {
                if (!m_jobQueue)
                    break;
                const JobQueue::RequestResult result = m_jobQueue->request(sessionId, blocker);
                if (gotJob(result))
                    hasJob = true;
                else if (wasRefused(result))
                    markRefused(sessionId, blocker);
            }
        }

        if (!hasJob) {
            // The chain ended (available, failed, or nothing requestable)
            const auto waiting = m_waiting.find(id);
            if (waiting != m_waiting.end() && waiting->contains(sessionId))
                waiting->insert(sessionId, false);
        }
    }
}

// A job of this session ended without success, or was asked to cancel: nothing
// continues and nothing is requested again. A track that still has another live
// job among its blockers keeps its flag.
void PlotRequests::stopContinuing(const QString &sessionId)
{
    QVector<PlotValue> plots;
    for (const QString &id : std::as_const(m_checkedOrder)) {
        if (m_waiting.value(id).value(sessionId, false))
            plots.append(m_checked.value(id));
    }
    if (plots.isEmpty())
        return;

    const LiveJobs live = liveJobs();
    const Inspections inspections = inspect(plots, sessionId);

    for (const PlotValue &plot : std::as_const(plots)) {
        const QString id = plotId(plot);
        const auto waiting = m_waiting.find(id);
        if (waiting == m_waiting.end())
            continue;
        const QList<Inspection> inspected = inspections.value(id);
        if (inspected.isEmpty()) {
            waiting->remove(sessionId);     // the session is gone, hidden, or unloaded
            continue;
        }
        const BlockerReport &report = inspected.first().report;
        const bool stillWaiting = report.state == BlockerReport::State::Blocked
            && hasLiveJob(sessionId, report, live);
        if (!stillWaiting)
            waiting->insert(sessionId, false);
    }
}

// ---- Cancel and pruning -----------------------------------------------------------------------

int PlotRequests::cancelPressed(const QString &plotId)
{
    if (isInert())
        return 0;
    if (syncCheckedSet())
        pruneUnwantedQueued();
    const auto checked = m_checked.constFind(plotId);
    if (checked == m_checked.constEnd())
        return 0;
    const PlotValue plot = checked.value();
    if (!isExplicitBacked(plot))
        return 0;

    // Collect first: cancel() re-enters the component through jobFinished
    const LiveJobs live = liveJobs();
    const QList<Inspection> inspected = inspect({plot}).value(plotId);
    QSet<JobId> found;
    for (const Inspection &inspection : inspected) {
        if (inspection.report.state != BlockerReport::State::Blocked)
            continue;
        for (const CalculationBlocker &blocker : inspection.report.blockers) {
            const auto job = live.constFind(JobKey(inspection.track.sessionId, blocker.instanceId));
            if (job != live.constEnd())
                found.insert(job->id);
        }
    }
    QList<JobId> ids(found.cbegin(), found.cend());
    std::sort(ids.begin(), ids.end());

    m_waiting.remove(plotId);

    int cancelled = 0;
    for (const JobId id : std::as_const(ids)) {
        if (m_jobQueue && m_jobQueue->cancel(id))
            ++cancelled;
    }

    // The running job is only asked to stop, but by the live-job rule its
    // tracks are Missing already - for this row and for every other row
    recompute();
    return cancelled;
}

// Queued jobs that no checked plot needs on a visible track end Cancelled. The
// queue never offers the running job to the predicate: its result is valid and
// worth keeping. "Wanted" is derived from checked plots x visible tracks only;
// who requested a job plays no part.
void PlotRequests::pruneUnwantedQueued()
{
    if (isInert())
        return;

    bool anyQueued = false;
    const QList<JobId> active = m_jobQueue->activeJobs();
    for (const JobId id : active) {
        if (m_jobQueue->job(id).state == JobState::Queued) {
            anyQueued = true;
            break;
        }
    }
    if (!anyQueued)
        return;

    QSet<JobKey> needed;
    const Inspections inspections = inspect(inspectedPlots());
    for (auto it = inspections.constBegin(); it != inspections.constEnd(); ++it) {
        for (const Inspection &inspection : it.value()) {
            for (const CalculationBlocker &blocker : inspection.report.blockers)
                needed.insert(JobKey(inspection.track.sessionId, blocker.instanceId));
        }
    }

    m_jobQueue->cancelUnwantedQueued([&needed](const JobRecord &record) {
        return needed.contains(JobKey(record.sessionId, record.instanceId));
    });
}

void PlotRequests::forgetSessions(const QSet<QString> &sessionIds)
{
    for (auto it = m_waiting.begin(); it != m_waiting.end(); ++it) {
        for (const QString &sessionId : sessionIds)
            it->remove(sessionId);
    }
}

// After a reset of the session model: sessions that are no longer visible,
// loaded tracks leave every waiting set at once, so that no continuation can be
// issued for them.
void PlotRequests::dropVanishedTracks()
{
    if (m_waiting.isEmpty() || !m_sessionModel)
        return;

    QSet<QString> tracks;
    {
        const SessionModel &model = *m_sessionModel;
        const SessionModel::RowStabilityGuard guard(model);
        const int rows = model.rowCount();
        for (int row = 0; row < rows; ++row) {
            const SessionRow &sr = model.rowAt(row);
            if (sr.isLoaded() && sr.visible && !sr.loadFailed)
                tracks.insert(sr.sessionId);
        }
    }

    for (auto it = m_waiting.begin(); it != m_waiting.end(); ++it) {
        for (auto entry = it->begin(); entry != it->end();) {
            if (tracks.contains(entry.key()))
                ++entry;
            else
                entry = it->erase(entry);
        }
    }
}

// ---- Slots ------------------------------------------------------------------------------------

void PlotRequests::onPlotDataChanged(const QModelIndex &, const QModelIndex &, const QList<int> &roles)
{
    if (roles.isEmpty() || roles.contains(Qt::CheckStateRole))
        onPlotCheckStateChanged();
}

// The single handler for every way a check state changes (a click, the Plots
// menu, a profile, startup restore, a reset). None of them is a gesture: a
// newly checked plot only schedules a pass.
void PlotRequests::onPlotCheckStateChanged()
{
    // m_checked is current at once, so that a gesture that follows in the same
    // call stack sees the plot as checked
    const bool anyUnchecked = syncCheckedSet();
    if (anyUnchecked)
        pruneUnwantedQueued();
    scheduleUpdate();
}

void PlotRequests::onDependencyChanged(const QString &sessionId, const DependencyKey &key)
{
    // Whatever the queue refused for this session may be possible now
    for (auto it = m_refused.begin(); it != m_refused.end();) {
        if (it->first == sessionId)
            it = m_refused.erase(it);
        else
            ++it;
    }

    if (m_relevantNamesDirty)
        rebuildRelevantNames();
    if (m_relevantNames.contains(key))
        scheduleUpdate();
}

void PlotRequests::onVisibilityChanged(const QSet<QString> &, const QSet<QString> &hidden)
{
    if (!hidden.isEmpty()) {
        forgetSessions(hidden);
        pruneUnwantedQueued();
    }
    scheduleUpdate();
}

void PlotRequests::onSessionModelReset()
{
    m_refused.clear();
    dropVanishedTracks();
    scheduleUpdate();
}

void PlotRequests::onJobFinished(JobId id, JobState state)
{
    // Read first: the record may be trimmed once this slot has returned
    const JobRecord record = m_jobQueue ? m_jobQueue->job(id) : JobRecord();
    if (record.id != 0) {
        if (state == JobState::Succeeded)
            continueAfter(record);
        else
            stopContinuing(record.sessionId);
    }
    scheduleUpdate();
}

void PlotRequests::onJobCancelRequested(JobId id)
{
    const JobRecord record = m_jobQueue ? m_jobQueue->job(id) : JobRecord();
    if (record.id != 0)
        stopContinuing(record.sessionId);
    scheduleUpdate();
}

// Text only, without inspection: optimizer iterations can arrive many times
// per second.
void PlotRequests::onJobProgress(JobId id, const QString &text)
{
    QStringList changed;
    for (auto it = m_states.begin(); it != m_states.end(); ++it) {
        PlotRowState &state = it.value();
        bool touched = false;
        for (PlotTrackState &track : state.pending) {
            if (track.job != id)
                continue;
            // Progress comes from the running job only
            if (track.jobState != JobState::Running || track.jobProgressText != text) {
                track.jobState = JobState::Running;
                track.jobProgressText = text;
                touched = true;
            }
        }
        if (!touched)
            continue;
        state.jobProgressText = text;
        state.toolTip = buildToolTip(state);
        changed.append(it.key());
    }

    std::sort(changed.begin(), changed.end());
    for (const QString &plotId : std::as_const(changed))
        emit rowStateChanged(plotId);
    if (!changed.isEmpty())
        emit rowStatesChanged();
}

void PlotRequests::onRegistryChanged()
{
    m_explicitBacked.clear();
    m_staticNames.clear();
    m_relevantNames.clear();
    m_relevantNamesDirty = true;
    m_refused.clear();
    scheduleUpdate();
}

} // namespace FlySight
