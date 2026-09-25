#include "calculationdemand.h"

#include <algorithm>
#include <limits>

#include <QDebug>
#include <QScopedValueRollback>

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

// A track is a row the plot widget draws: loaded, visible, and not a
// failed-load placeholder. Call under a RowStabilityGuard.
bool isVisibleLoadedTrack(const SessionRow &row)
{
    return row.isLoaded() && row.visible && !row.loadFailed;
}

} // namespace

// ---- Construction ---------------------------------------------------------------

CalculationDemand::CalculationDemand(SessionModel *sessionModel, PlotModel *plotModel, JobQueue *executor,
                                     QObject *parent)
    : QObject(parent)
    , m_sessionModel(sessionModel)
    , m_plotModel(plotModel)
    , m_queue(executor)
{
    m_updateTimer.setSingleShot(true);
    m_updateTimer.setInterval(0);
    connect(&m_updateTimer, &QTimer::timeout, this, &CalculationDemand::recompute);

    m_settleTimer.setSingleShot(true);
    connect(&m_settleTimer, &QTimer::timeout, this, &CalculationDemand::onSettleTimeout);

    if (m_queue) {
        // After a start the chosen next slot is empty, and after a cancel
        // request the running pair is waiting again: the pass fills the slot
        connect(m_queue, &JobQueue::jobStarted, this, &CalculationDemand::scheduleUpdate);
        connect(m_queue, &JobQueue::jobCancelRequested, this, &CalculationDemand::scheduleUpdate);
        connect(m_queue, &JobQueue::jobFinished, this, &CalculationDemand::onJobFinished);
        connect(m_queue, &JobQueue::jobProgress, this, &CalculationDemand::onJobProgress);
    }

    if (m_sessionModel) {
        connect(m_sessionModel, &SessionModel::dependencyChanged, this, &CalculationDemand::onDependencyChanged);
        connect(m_sessionModel, &SessionModel::visibilityChanged, this, &CalculationDemand::onVisibilityChanged);
        connect(m_sessionModel, &SessionModel::modelChanged, this, &CalculationDemand::scheduleUpdate);
        // The background loader announces visibility only at the end of a
        // batch; a track becomes one the moment its session is loaded
        connect(m_sessionModel, &SessionModel::sessionLoaded, this,
                [this](const QString &) { scheduleUpdate(); });
        connect(m_sessionModel, &SessionModel::focusedSessionChanged, this,
                [this](const QString &) { scheduleUpdate(); });
        connect(m_sessionModel, &QAbstractItemModel::modelReset, this, &CalculationDemand::onSessionModelReset);
    }

    if (m_plotModel) {
        connect(m_plotModel, &QAbstractItemModel::dataChanged, this, &CalculationDemand::onPlotDataChanged);
        connect(m_plotModel, &QAbstractItemModel::modelReset, this, &CalculationDemand::onPlotCheckStateChanged);
    }

    // The observer only clears memos and memory and starts the timer: it runs
    // synchronously inside the registration, possibly during plugin loading
    m_registryObserver = CalculationRegistry::instance().addObserver([this] { onRegistryChanged(); });

    // Plots restored as checked create demand without any event (at start-up
    // no session is visible yet, so the first pass offers nothing)
    syncCheckedSet();
    scheduleUpdate();
}

CalculationDemand::~CalculationDemand()
{
    CalculationRegistry::instance().removeObserver(m_registryObserver);
}

QString CalculationDemand::plotId(const QString &sensorId, const QString &measurementId)
{
    return sensorId + QLatin1Char('/') + measurementId;
}

QString CalculationDemand::plotId(const PlotValue &plot)
{
    return plotId(plot.sensorID, plot.measurementID);
}

DemandState CalculationDemand::plotState(const QString &plotId) const
{
    return m_states.value(plotId);
}

bool CalculationDemand::isInert() const
{
    return !m_sessionModel || !m_plotModel || !m_queue;
}

// ---- Which plots matter -----------------------------------------------------------

// The registry is the one authority (CalculationRegistry::dependsOnExplicit());
// the memo here is per plot id and also keeps the static names of the plot.
bool CalculationDemand::isRequested(const PlotValue &plot)
{
    const QString id = plotId(plot);
    const auto known = m_requested.constFind(id);
    if (known != m_requested.constEnd())
        return known.value();

    const CalculationRegistry &registry = CalculationRegistry::instance();
    const QSet<DependencyKey> names = registry.staticDependencies(yName(plot)).names;
    const bool requested = registry.dependsOnExplicit(yName(plot));

    m_requested.insert(id, requested);
    m_staticNames.insert(id, names);
    return requested;
}

// Brings m_checked in line with the PlotModel.
bool CalculationDemand::syncCheckedSet()
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
            break;
        }
    }

    if (order != m_checkedOrder)
        m_relevantNamesDirty = true;
    m_checked = checked;
    m_checkedOrder = order;
    return anyUnchecked;
}

void CalculationDemand::rebuildRelevantNames()
{
    m_relevantNames.clear();
    for (const QString &id : std::as_const(m_checkedOrder)) {
        if (isRequested(m_checked.value(id)))
            m_relevantNames.unite(m_staticNames.value(id));
    }
    m_relevantNamesDirty = false;
}

QVector<PlotValue> CalculationDemand::inspectedPlots()
{
    QVector<PlotValue> plots;
    for (const QString &id : std::as_const(m_checkedOrder)) {
        const PlotValue plot = m_checked.value(id);
        if (isRequested(plot))
            plots.append(plot);
    }
    return plots;
}

// ---- Inspection and classification ---------------------------------------------------

bool CalculationDemand::isMerelyUncomputed(const SessionData &session, const QString &sensorId,
                                           const QString &measurementId)
{
    using State = BlockerReport::State;
    const State state = session.calculationEngine()
        .blockers(DependencyKey::measurement(sensorId, measurementId)).state;
    return state == State::Blocked || state == State::NotProduced;
}

// Call under a RowStabilityGuard: `session` is a row of the model read in place.
BlockerReport CalculationDemand::inspectUnderGuard(const SessionData &session, const PlotValue &plot) const
{
    const BlockerReport report = session.calculationEngine().blockers(yName(plot));

#ifndef QT_NO_DEBUG
    // "y available implies x available" (see the class comment) is a property
    // of the registrations that nothing else checks. Which time axis is drawn
    // is a view setting, so the registry cannot check it at registration; both
    // axes are inspected here instead. blockers() never starts explicit work
    // and never loads a session. A time axis that is unavailable for ordinary
    // reasons is not a violation: only one that waits on an explicit
    // calculation would leave a Done track undrawn.
    if (report.state == BlockerReport::State::Available) {
        for (const char *axis : {SessionKeys::Time, SessionKeys::SystemTime}) {
            const DependencyKey xName = DependencyKey::measurement(plot.sensorID, QLatin1String(axis));
            if (session.calculationEngine().blockers(xName).state == BlockerReport::State::Blocked) {
                qWarning().noquote() << "CalculationDemand:" << plotId(plot)
                                     << "is available but its time axis" << QLatin1String(axis)
                                     << "waits on an explicit calculation";
            }
        }
    }
#endif

    return report;
}

// The one guarded read of the component. The tracks are the rows the plot
// widget draws: loaded, visible, and not a failed-load placeholder, in row
// order. Everything returned is a plain value, so that nothing is offered,
// withdrawn, or emitted while the guard is held. blockers() may compute
// on-demand values; that reads, it neither loads nor evicts.
CalculationDemand::Inspections CalculationDemand::inspect(const QVector<PlotValue> &plots) const
{
    Inspections result;
    if (!m_sessionModel || plots.isEmpty())
        return result;

    const SessionModel &model = *m_sessionModel;
    const SessionModel::RowStabilityGuard guard(model);
    const int rows = model.rowCount();
    for (int row = 0; row < rows; ++row) {
        const SessionRow &sr = model.rowAt(row);
        if (!isVisibleLoadedTrack(sr))
            continue;

        const SessionData &session = sr.session.value();
        Track track;
        track.sessionId = sr.sessionId;
        track.sessionName = session.getAttribute(SessionKeys::Description).toString();
        if (track.sessionName.isEmpty())
            track.sessionName = sr.sessionId;

        for (const PlotValue &plot : plots)
            result[plotId(plot)].append(Inspection{track, inspectUnderGuard(session, plot)});
    }
    return result;
}

QString CalculationDemand::failureReason(const QList<UnproducedNote> &notes)
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

// `running` and `chosen` are the executor's running and chosen next records
// (default records when there are none).
DemandTrack CalculationDemand::classify(const Track &track, const BlockerReport &report,
                                        const JobRecord &running, const JobRecord &chosen) const
{
    DemandTrack state;
    state.sessionId = track.sessionId;
    state.sessionName = track.sessionName;

    switch (report.state) {
    case BlockerReport::State::Available:
        state.condition = DemandCondition::Done;
        return state;
    case BlockerReport::State::NotApplicable:
        state.condition = DemandCondition::NotApplicable;
        return state;
    case BlockerReport::State::NotProduced:
        state.condition = DemandCondition::Failed;
        for (const UnproducedNote &note : report.notProduced)
            state.calculationTitles.append(titleOf(note.calculation));
        state.reason = failureReason(report.notProduced);
        return state;
    case BlockerReport::State::Blocked:
        break;      // notes on a Blocked report are ignored: Blocked wins
    }

    const bool runningIsLive = running.id != 0 && !running.cancelRequested
        && running.sessionId == track.sessionId;
    bool isRunning = false;
    bool isChosen = false;
    bool allNotApplicable = true;
    QStringList titles;             // the blockers not remembered as not applicable
    QStringList failedTitles;
    QStringList failedReasons;
    for (const CalculationBlocker &blocker : report.blockers) {
        const PairKey key(track.sessionId, blocker.instanceId);
        if (runningIsLive && running.instanceId == blocker.instanceId)
            isRunning = true;
        if (chosen.id != 0 && chosen.sessionId == track.sessionId && chosen.instanceId == blocker.instanceId)
            isChosen = true;

        const auto memory = m_memory.constFind(key);
        if (memory != m_memory.constEnd() && memory->kind == Memory::Kind::JobFailed) {
            failedTitles.append(titleOf(blocker));
            failedReasons.append(memory->reason);
        }
        if (memory == m_memory.constEnd() || memory->kind != Memory::Kind::NotApplicable) {
            allNotApplicable = false;
            titles.append(titleOf(blocker));
        }
    }

    if (isRunning) {
        state.condition = DemandCondition::Running;
        state.calculationTitles = titles;
        state.job = running.id;
        state.progressText = running.progressText;
    } else if (!failedReasons.isEmpty()) {
        // Not offered again in this run: the badge says why
        state.condition = DemandCondition::Failed;
        state.jobFailure = true;
        state.calculationTitles = failedTitles;
        state.reason = failedReasons.join(QStringLiteral("; "));
    } else if (allNotApplicable) {
        // The executor said there is nothing to run for any of them
        state.condition = DemandCondition::NotApplicable;
    } else {
        state.condition = DemandCondition::Waiting;
        state.calculationTitles = titles;
        state.settling = isSettling(track.sessionId);
        state.job = isChosen ? chosen.id : 0;
    }
    return state;
}

// ---- The choice -------------------------------------------------------------------------

// Tier (a), the focused track's pairs, then tier (b), the other tracks' pairs in
// row order. Within a session: plot-model order, then the report's blocker
// order (upstream first), each pair once.
QList<CalculationDemand::Candidate> CalculationDemand::plotCandidates(const Inspections &inspections,
                                                                      const QString &focusedId) const
{
    QList<Candidate> candidates;
    if (!m_queue)
        return candidates;

    // The plots in plot-model order, and the tracks in row order (every
    // inspected plot has the same tracks)
    QStringList plots;
    for (const QString &id : m_checkedOrder) {
        if (inspections.contains(id))
            plots.append(id);
    }
    if (plots.isEmpty())
        return candidates;
    QStringList sessions;
    for (const Inspection &inspection : inspections.value(plots.first()))
        sessions.append(inspection.track.sessionId);
    if (sessions.contains(focusedId)) {
        sessions.removeOne(focusedId);
        sessions.prepend(focusedId);
    }

    PairKey runningKey;
    if (const JobId runningId = m_queue->runningJob()) {
        const JobRecord running = m_queue->job(runningId);
        if (running.id != 0 && !running.cancelRequested)
            runningKey = PairKey(running.sessionId, running.instanceId);
    }

    QSet<PairKey> seen;
    for (const QString &sessionId : std::as_const(sessions)) {
        if (isSettling(sessionId))
            continue;       // offered once its inputs have been still for the whole wait
        for (const QString &plot : std::as_const(plots)) {
            for (const Inspection &inspection : inspections.value(plot)) {
                if (inspection.track.sessionId != sessionId)
                    continue;
                if (inspection.report.state != BlockerReport::State::Blocked)
                    break;
                for (const CalculationBlocker &blocker : inspection.report.blockers) {
                    const PairKey key(sessionId, blocker.instanceId);
                    if (seen.contains(key))
                        continue;
                    seen.insert(key);
                    if (m_memory.contains(key) || key == runningKey)
                        continue;
                    candidates.append(Candidate{sessionId, blocker});
                }
                break;      // one inspection per (plot, session)
            }
        }
    }
    return candidates;
}

// Keeps the executor's chosen next job equal to the first candidate it
// accepts. The only place that offers.
void CalculationDemand::offerChoice(const QList<Candidate> &candidates)
{
    using Kind = JobQueue::OfferResult::Kind;
    if (!m_queue)
        return;

    for (const Candidate &candidate : candidates) {
        const PairKey key(candidate.sessionId, candidate.calculation.instanceId);
        if (const JobId chosen = m_queue->chosenNextJob()) {
            const JobRecord record = m_queue->job(chosen);
            if (PairKey(record.sessionId, record.instanceId) == key) {
                m_offeredJob = chosen;      // already the choice: nothing changes
                return;
            }
        }

        const JobQueue::OfferResult result = m_queue->offer(candidate.sessionId, candidate.calculation);
        switch (result.kind) {
        case Kind::Created:
            m_offeredJob = result.job;
            return;
        case Kind::MissingInput:
        case Kind::NothingToDo:
        case Kind::UnknownCalculation:
            // There will never be a job for it in this run, unless its
            // session's inputs change
            m_memory.insert(key, Memory{Memory::Kind::NotApplicable, QString()});
            continue;
        case Kind::Blocked:
        case Kind::SessionNotLoaded:
        case Kind::AlreadyActive:
            continue;
        case Kind::ShuttingDown:
            return;
        }
    }

    // Nothing to choose: demand no longer wants what this component offered
    withdrawOwnOffer();
}

// A chosen next job this component did not offer is left alone.
void CalculationDemand::withdrawOwnOffer()
{
    if (!m_queue)
        return;
    const JobId chosen = m_queue->chosenNextJob();
    if (chosen != 0 && chosen == m_offeredJob)
        m_queue->withdrawChosenNext();
}

// ---- The pass -------------------------------------------------------------------------

void CalculationDemand::scheduleUpdate()
{
    if (m_updateTimer.isActive())
        return;
    m_updateTimer.start();
}

void CalculationDemand::flush()
{
    if (m_updateTimer.isActive())
        recompute();
}

bool CalculationDemand::hasPendingUpdate() const
{
    return m_updateTimer.isActive();
}

QString CalculationDemand::buildToolTip(const DemandState &state)
{
    const QString indent = QStringLiteral("  ");
    QStringList lines;

    if (state.isWorking()) {
        lines.append(tr("Computing: %1 of %2 done").arg(state.doneCount).arg(state.wantedCount));
        for (const DemandTrack &track : state.running) {
            const QString progress = track.progressText.isEmpty() ? tr("running") : track.progressText;
            lines.append(indent + tr("%1 - %2: %3").arg(track.sessionName,
                                                         track.calculationTitles.join(QStringLiteral(", ")),
                                                         progress));
        }
    }
    if (state.failedCount > 0) {
        lines.append(tr("Could not be computed:"));
        for (const DemandTrack &track : state.failed)
            lines.append(indent + tr("%1 - %2").arg(track.sessionName, track.reason));
    }
    return lines.join(QLatin1Char('\n'));
}

// Aggregates one inspected source. `tracks` are in row order.
DemandState CalculationDemand::buildState(const QString &sourceId, const QList<DemandTrack> &tracks) const
{
    DemandState state;
    state.sourceId = sourceId;
    state.requested = true;

    for (const DemandTrack &track : tracks) {
        switch (track.condition) {
        case DemandCondition::Done:
            ++state.wantedCount;
            ++state.doneCount;
            break;
        case DemandCondition::Waiting:
            ++state.wantedCount;
            state.waiting.append(track);
            break;
        case DemandCondition::Running:
            ++state.wantedCount;
            state.running.append(track);
            break;
        case DemandCondition::Failed:
            ++state.wantedCount;
            ++state.doneCount;
            state.failed.append(track);
            break;
        case DemandCondition::NotApplicable:
            break;      // silently absent
        }
    }
    state.waitingCount = int(state.waiting.size());
    state.runningCount = int(state.running.size());
    state.failedCount = int(state.failed.size());

    if (state.isWorking())
        state.progressLabel = tr("%1 of %2").arg(state.doneCount).arg(state.wantedCount);
    state.toolTip = buildToolTip(state);
    return state;
}

// Stores the new states and announces the differences. `order` lists the
// inspected plots; a plot that is no longer inspected falls back to the default
// state and is announced once.
void CalculationDemand::applyStates(const QStringList &order, const QHash<QString, DemandState> &states)
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

    // Stored before anything is emitted: a slot reads plotState()
    m_states = states;

    for (const QString &id : std::as_const(changed))
        emit plotStateChanged(id);
    if (!changed.isEmpty())
        emit statesChanged();
}

// The one pass: every path that reconciles runs it. Classifications are not
// cached across passes: the engine's own caches make a repeated blockers()
// cheap, and a cache keyed on dependencyChanged would be wrong (after A
// publishes, the blocker of B's output changes from A to B although B's output
// may not be re-announced).
void CalculationDemand::recompute()
{
    m_updateTimer.stop();
    ++m_passCount;

    QStringList order;
    QHash<QString, DemandState> states;
    {
        // The executor's signals caused by this pass's own offer or withdrawal
        // only schedule another pass
        const QScopedValueRollback<bool> reconciling(m_reconciling, true);
        dropExpiredSettles();

        if (!isInert())
            syncCheckedSet();
        const QVector<PlotValue> plots = isInert() ? QVector<PlotValue>() : inspectedPlots();
        if (plots.isEmpty()) {
            // Ordinary plots cost nothing: sessions are not even enumerated.
            // Demand is empty.
            withdrawOwnOffer();
        } else {
            const Inspections inspections = inspect(plots);
            // The guard is gone: from here on the executor may be called

            if (!m_queue->isShutDown()) {
                QString focused = m_sessionModel->focusedSessionId();
                offerChoice(plotCandidates(inspections, focused));
            }

            // After the offers: the executor's jobs as they are now
            const JobRecord running = m_queue->job(m_queue->runningJob());
            const JobRecord chosen = m_queue->job(m_queue->chosenNextJob());
            for (const PlotValue &plot : plots) {
                const QString id = plotId(plot);
                QList<DemandTrack> tracks;
                const QList<Inspection> inspected = inspections.value(id);
                for (const Inspection &inspection : inspected)
                    tracks.append(classify(inspection.track, inspection.report, running, chosen));
                order.append(id);
                states.insert(id, buildState(id, tracks));
            }
        }
    }

    applyStates(order, states);
}

// ---- The input-settle wait ------------------------------------------------------------

void CalculationDemand::setInputSettleDelay(int milliseconds)
{
    m_settleDelayMs = qMax(0, milliseconds);
}

void CalculationDemand::endInputSettleWaits()
{
    m_settleUntil.clear();
    m_settleTimer.stop();
    scheduleUpdate();
}

bool CalculationDemand::isSettling(const QString &sessionId) const
{
    const auto it = m_settleUntil.constFind(sessionId);
    return it != m_settleUntil.constEnd() && !it->hasExpired();
}

bool CalculationDemand::hasSettlingSessions() const
{
    return std::any_of(m_settleUntil.cbegin(), m_settleUntil.cend(),
                       [](const QDeadlineTimer &deadline) { return !deadline.hasExpired(); });
}

void CalculationDemand::dropExpiredSettles()
{
    for (auto it = m_settleUntil.begin(); it != m_settleUntil.end();) {
        if (it->hasExpired())
            it = m_settleUntil.erase(it);
        else
            ++it;
    }
}

void CalculationDemand::armSettleTimer()
{
    qint64 earliest = -1;
    for (const QDeadlineTimer &deadline : std::as_const(m_settleUntil)) {
        const qint64 remaining = qMax<qint64>(0, deadline.remainingTime());
        if (earliest < 0 || remaining < earliest)
            earliest = remaining;
    }
    if (earliest < 0)
        m_settleTimer.stop();
    else
        m_settleTimer.start(int(qMin<qint64>(earliest, std::numeric_limits<int>::max())));
}

void CalculationDemand::onSettleTimeout()
{
    dropExpiredSettles();
    armSettleTimer();
    scheduleUpdate();
}

// ---- Slots ------------------------------------------------------------------------------------

void CalculationDemand::onPlotDataChanged(const QModelIndex &, const QModelIndex &, const QList<int> &roles)
{
    if (roles.isEmpty() || roles.contains(Qt::CheckStateRole))
        onPlotCheckStateChanged();
}

// The single handler for every way a check state changes (a click, the Plots
// menu, a profile, the start-up restore, a reset): all create demand alike.
void CalculationDemand::onPlotCheckStateChanged()
{
    // m_checked is current at once
    const bool anyUnchecked = syncCheckedSet();
    // An unchecked plot drops its waiting pair before it can start
    if (anyUnchecked && m_queue && m_queue->chosenNextJob() != 0)
        recompute();
    else
        scheduleUpdate();
}

void CalculationDemand::onDependencyChanged(const QString &sessionId, const DependencyKey &key)
{
    // Only the static closure of the checked requested plots matters: any other
    // edit neither delays nor retries anything
    if (m_relevantNamesDirty)
        rebuildRelevantNames();
    if (!m_relevantNames.contains(key))
        return;

    // A job's own publication is not an input change: the synchronous pass in
    // jobFinished continues a chain without a wait
    if (m_queue) {
        const JobId publishing = m_queue->publishingJob();
        if (publishing != 0 && m_queue->job(publishing).sessionId == sessionId) {
            scheduleUpdate();
            return;
        }
    }

    // An input change: whatever was remembered for the session may be
    // possible now, and the session waits until its inputs are still
    for (auto it = m_memory.begin(); it != m_memory.end();) {
        if (it.key().first == sessionId)
            it = m_memory.erase(it);
        else
            ++it;
    }
    m_settleUntil.insert(sessionId, QDeadlineTimer(m_settleDelayMs));
    armSettleTimer();
    scheduleUpdate();
}

void CalculationDemand::onVisibilityChanged(const QSet<QString> &, const QSet<QString> &hidden)
{
    // A hidden session drops its waiting pair before it can start
    if (!hidden.isEmpty() && m_queue && m_queue->chosenNextJob() != 0)
        recompute();
    else
        scheduleUpdate();
}

// A sort resets the model too: only the ids that no longer have a row are
// forgotten, so that a job failure is not retried on every sort.
void CalculationDemand::onSessionModelReset()
{
    if (m_sessionModel) {
        const SessionModel &model = *m_sessionModel;
        for (auto it = m_memory.begin(); it != m_memory.end();) {
            if (model.getSessionRow(it.key().first) < 0)
                it = m_memory.erase(it);
            else
                ++it;
        }
        for (auto it = m_settleUntil.begin(); it != m_settleUntil.end();) {
            if (model.getSessionRow(it.key()) < 0)
                it = m_settleUntil.erase(it);
            else
                ++it;
        }
        armSettleTimer();
    }
    scheduleUpdate();
}

void CalculationDemand::onJobFinished(JobId id, JobState state)
{
    // Read first: the record may be trimmed once this slot has returned
    const JobRecord record = m_queue ? m_queue->job(id) : JobRecord();
    if (state == JobState::Failed && record.id != 0) {
        // Not a function of the inputs, and not stored: not offered again in
        // this run unless the session's inputs change
        m_memory.insert(PairKey(record.sessionId, record.instanceId),
                        Memory{Memory::Kind::JobFailed, record.calculationTitle + QStringLiteral(": ") + record.reason});
    }

    // An end caused by this component's own offer or withdrawal
    if (m_reconciling) {
        scheduleUpdate();
        return;
    }
    // Synchronously: the executor emits jobFinished before it decides between
    // idle() and the next start, so the next choice is in place first
    recompute();
}

// Text only, without inspection: optimizer iterations can arrive many times
// per second.
void CalculationDemand::onJobProgress(JobId id, const QString &text)
{
    QStringList changed;
    for (auto it = m_states.begin(); it != m_states.end(); ++it) {
        DemandState &state = it.value();
        bool touched = false;
        for (DemandTrack &track : state.running) {
            if (track.job != id || track.progressText == text)
                continue;
            track.progressText = text;
            touched = true;
        }
        if (!touched)
            continue;
        state.toolTip = buildToolTip(state);
        changed.append(it.key());
    }

    std::sort(changed.begin(), changed.end());
    for (const QString &plotId : std::as_const(changed))
        emit plotStateChanged(plotId);
    if (!changed.isEmpty())
        emit statesChanged();
}

void CalculationDemand::onRegistryChanged()
{
    m_requested.clear();
    m_staticNames.clear();
    m_relevantNames.clear();
    m_relevantNamesDirty = true;
    m_memory.clear();
    scheduleUpdate();
}

} // namespace FlySight
