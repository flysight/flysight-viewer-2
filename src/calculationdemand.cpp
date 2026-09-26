#include "calculationdemand.h"

#include <algorithm>

#include <QDebug>
#include <QScopedValueRollback>

#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "demandfill.h"
#include "demandsettleclock.h"
#include "jobqueue.h"
#include "logbookmanager.h"
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

// Nothing is left to compute for the cell (see SETTLEMENTS)
bool isFinal(DemandCondition condition)
{
    return condition == DemandCondition::Done || condition == DemandCondition::Failed
        || condition == DemandCondition::NotApplicable;
}

// The cell is in demand
bool isPending(DemandCondition condition)
{
    return condition == DemandCondition::Waiting || condition == DemandCondition::Running;
}

// A listed track of a state (running or failed) carries the session's name
bool isListed(DemandCondition condition)
{
    return condition == DemandCondition::Running || condition == DemandCondition::Failed;
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
    // The parts first: the slots below and the pass use them. Their callbacks
    // capture this component (see the destructor).
    m_settle = std::make_unique<DemandSettleClock>(kInputSettleMs, [this] { scheduleUpdate(); });
    m_fill = std::make_unique<DemandFill>(m_sessionModel.data(), DemandFill::Hooks{
        /*enabled*/        [this] { return !isInert() && !m_queue->isShutDown(); },
        /*runPendingPass*/ [this] { if (hasPendingUpdate()) recompute(); },
        /*runPass*/        [this] { recompute(); },
        /*loadFailed*/     [this](const QString &id) { onFillLoadFailed(id); },
        /*loaded*/         [this](const QString &id, const QString &held) { onFillLoaded(id, held); }});

    m_updateTimer.setSingleShot(true);
    m_updateTimer.setInterval(0);
    connect(&m_updateTimer, &QTimer::timeout, this, &CalculationDemand::recompute);

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
        // batch; a track becomes one the moment its session is loaded. The
        // load restored the session's stored results.
        connect(m_sessionModel, &SessionModel::sessionLoaded, this, [this](const QString &sessionId) {
            m_columnReports.remove(sessionId);
            scheduleUpdate();
        });
        connect(m_sessionModel, &SessionModel::focusedSessionChanged, this,
                [this](const QString &) { scheduleUpdate(); });
        // A column change resets the model too (SessionModel::rebuildColumns).
        // The executor's reset slot runs first and may end a job, whose
        // jobFinished runs a pass at once: the memos go before the reset.
        connect(m_sessionModel, &QAbstractItemModel::modelAboutToBeReset,
                this, &CalculationDemand::onSessionModelAboutToBeReset);
        connect(m_sessionModel, &QAbstractItemModel::modelReset, this, &CalculationDemand::onSessionModelReset);
    }

    // Direct: emitted from inside record methods, so the slot only drops state
    // and schedules, like SessionModel's own
    connect(&LogbookManager::instance(), &LogbookManager::calculationRecordsChanged,
            this, &CalculationDemand::onCalculationRecordsChanged);

    if (m_plotModel) {
        connect(m_plotModel, &QAbstractItemModel::dataChanged, this, &CalculationDemand::onPlotDataChanged);
        connect(m_plotModel, &QAbstractItemModel::modelReset, this, &CalculationDemand::onPlotCheckStateChanged);
    }

    // The observer only clears memos and memory and starts the timer: it runs
    // synchronously inside the registration, possibly during plugin loading
    m_registryObserver = CalculationRegistry::instance().addObserver([this] { onRegistryChanged(); });

    // The column fill: the lowest-priority task of the session model's
    // scheduler, whose steps are hidden loads
    if (!isInert())
        m_fill->registerTask();

    // Plots restored as checked and columns enabled create demand without any
    // event (at start-up no session is visible yet, so plots offer nothing;
    // enabled requested columns do create demand)
    syncCheckedSet();
    scheduleUpdate();
}

CalculationDemand::~CalculationDemand()
{
    CalculationRegistry::instance().removeObserver(m_registryObserver);
    // The fill's task and hooks capture this component: the task goes, then
    // the holds, before any member does
    m_fill->detach();
}

QString CalculationDemand::plotId(const QString &sensorId, const QString &measurementId)
{
    return sensorId + QLatin1Char('/') + measurementId;
}

QString CalculationDemand::plotId(const PlotValue &plot)
{
    return plotId(plot.sensorID, plot.measurementID);
}

QString CalculationDemand::columnId(const LogbookColumn &column)
{
    return logbookColumnDefinitionKey(column);
}

DemandState CalculationDemand::plotState(const QString &plotId) const
{
    return m_states.value(plotId);
}

DemandState CalculationDemand::columnState(const QString &columnId) const
{
    return m_columnStates.value(columnId);
}

bool CalculationDemand::isCellPending(const QString &sessionId, const QString &columnId) const
{
    const auto cells = m_pendingCells.constFind(columnId);
    return cells != m_pendingCells.constEnd() && cells->contains(sessionId);
}

// Painted for every cell: the ids are computed on every call, so that a stale
// mapping can never answer. Nothing is computed while no cell is pending.
bool CalculationDemand::isCellPending(int row, int column) const
{
    if (!m_hasPendingCells || !m_sessionModel || row < 0 || row >= m_sessionModel->rowCount()
        || column < 0 || column >= m_sessionModel->columnCount())
        return false;
    const SessionModel &model = *m_sessionModel;
    return isCellPending(model.rowAt(row).sessionId, columnId(model.column(column)));
}

QStringList CalculationDemand::workingPlotIds() const
{
    QStringList ids;
    for (auto it = m_states.constBegin(); it != m_states.constEnd(); ++it) {
        if (it.value().isWorking())
            ids.append(it.key());
    }
    return ids;
}

QStringList CalculationDemand::workingColumnIds() const
{
    QStringList ids;
    for (auto it = m_columnStates.constBegin(); it != m_columnStates.constEnd(); ++it) {
        if (it.value().isWorking())
            ids.append(it.key());
    }
    return ids;
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

    m_checked = checked;
    m_checkedOrder = order;
    return anyUnchecked;
}

// A name matters when it is in the static closure of a checked requested plot
// (computed here: the model knows nothing of plots) or of a requested column
// (read from the session model, which is current under the registrations).
// Ordinary columns do not count: an edit that affects no requested
// calculation starts no settle wait.
bool CalculationDemand::isRelevantName(const DependencyKey &key)
{
    for (const QString &id : std::as_const(m_checkedOrder)) {
        if (isRequested(m_checked.value(id)) && m_staticNames.value(id).contains(key))
            return true;
    }
    if (m_sessionModel) {
        const SessionModel &model = *m_sessionModel;
        const int count = model.columnCount();
        for (int i = 0; i < count; ++i) {
            if (!model.columnRequestedCalculations(i).isEmpty()
                && model.columnDependencyClosure(i).names.contains(key))
                return true;
        }
    }
    return false;
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

// Brings m_columns in line with the requested enabled columns as the session
// model reports them now (SessionModel::columnRequestedCalculations(): a
// column is requested when it is not empty). Runs at the start of every pass:
// a per-pass copy, read and not computed.
void CalculationDemand::syncColumns()
{
    QVector<ColumnInfo> columns;
    if (m_sessionModel) {
        const CalculationRegistry &registry = CalculationRegistry::instance();
        const SessionModel &model = *m_sessionModel;
        const int count = model.columnCount();
        for (int i = 0; i < count; ++i) {
            const LogbookColumn &column = model.column(i);
            const QStringList calculations = model.columnRequestedCalculations(i);
            if (calculations.isEmpty())
                continue;
            ColumnInfo info;
            info.id = columnId(column);
            info.names = logbookColumnNames(column);
            info.calculations = calculations;
            for (const QString &id : calculations) {
                // An explicit family instance is never stored
                if (id.contains(QLatin1Char('#')))
                    continue;
                const QString title = registry.title(id);
                info.storable.append(id);
                info.storableTitles.append(title.isEmpty() ? id : title);
            }
            columns.append(info);
        }
    }

    QStringList before;
    for (const ColumnInfo &column : std::as_const(m_columns))
        before.append(column.id);
    QStringList after;
    for (const ColumnInfo &column : std::as_const(columns))
        after.append(column.id);

    m_columns = columns;
    if (before == after)
        return;

    // The report memo is parallel to the columns; the settlements of a column
    // that left the set go (its state is announced once by the next pass)
    m_columnReports.clear();
    const QSet<QString> present(after.cbegin(), after.cend());
    for (auto it = m_settled.begin(); it != m_settled.end();) {
        if (present.contains(it.key().second))
            ++it;
        else
            it = m_settled.erase(it);
    }
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
        track.sessionName = model.sessionDisplayName(row);

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

// `running` is the executor's running record (a default record when there is
// none).
DemandTrack CalculationDemand::classify(const Track &track, const BlockerReport &report,
                                        const JobRecord &running) const
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
    bool allNotApplicable = true;
    QStringList titles;             // the blockers not remembered as not applicable
    QStringList failedTitles;
    QStringList failedReasons;
    for (const CalculationBlocker &blocker : report.blockers) {
        const PairKey key(track.sessionId, blocker.instanceId);
        if (runningIsLive && running.instanceId == blocker.instanceId)
            isRunning = true;

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
    }
    return state;
}

// ---- Column demand ------------------------------------------------------------------------

// Call under a RowStabilityGuard: `session` is a row of the model read in place.
// One report per column, combined over the column's names (two for a Delta):
// a value that needs both names exists only when both do.
QVector<BlockerReport> CalculationDemand::columnReports(const SessionData &session,
                                                       const QVector<ColumnInfo> &columns)
{
    using State = BlockerReport::State;
    QVector<BlockerReport> reports;
    reports.reserve(columns.size());
    auto &engine = session.calculationEngine();

    for (const ColumnInfo &column : columns) {
        bool notApplicable = false;
        bool notProduced = false;
        bool blocked = false;
        BlockerReport combined;
        QSet<QString> seenBlockers;
        QSet<QString> seenNotes;
        for (const DependencyKey &name : column.names) {
            const BlockerReport report = engine.blockers(name);
            switch (report.state) {
            case State::Available:
                break;
            case State::NotApplicable:
                notApplicable = true;
                break;
            case State::NotProduced:
                notProduced = true;
                for (const UnproducedNote &note : report.notProduced) {
                    if (!seenNotes.contains(note.calculation.instanceId)) {
                        seenNotes.insert(note.calculation.instanceId);
                        combined.notProduced.append(note);
                    }
                }
                break;
            case State::Blocked:
                blocked = true;
                for (const CalculationBlocker &blocker : report.blockers) {
                    if (!seenBlockers.contains(blocker.instanceId)) {
                        seenBlockers.insert(blocker.instanceId);
                        combined.blockers.append(blocker);
                    }
                }
                break;
            }
        }

        if (notApplicable) {
            combined = BlockerReport();
            combined.state = State::NotApplicable;
        } else if (notProduced) {
            combined.state = State::NotProduced;
            combined.blockers.clear();
        } else if (blocked) {
            combined.state = State::Blocked;
            combined.notProduced.clear();
        } else {
            combined = BlockerReport();
            combined.state = State::Available;
        }
        reports.append(combined);
    }
    return reports;
}

// The one guarded read of column demand. It reads only: it writes this
// component's memos and settlements, and emits, offers, pins and loads nothing.
// `running` is the executor's running record before the offers; `held` is a
// copy of the fill's holds.
CalculationDemand::ColumnWalk CalculationDemand::walkColumns(const JobRecord &running, const QSet<QString> &held)
{
    ColumnWalk walk;
    const int columns = int(m_columns.size());
    walk.states.resize(columns);
    walk.pendingCells.resize(columns);

    PairKey runningKey;
    if (running.id != 0 && !running.cancelRequested)
        runningKey = PairKey(running.sessionId, running.instanceId);

    // Adds one cell to the column's tallies
    const auto tally = [&walk](int column, const QString &sessionId, const DemandTrack &track) {
        walk.states[column].addTrack(track);
        if (isPending(track.condition)) {
            walk.pendingCells[column].insert(sessionId);
            walk.pendingSessions.insert(sessionId);
        }
    };

    const SessionModel &model = *m_sessionModel;
    const SessionModel::RowStabilityGuard guard(model);
    const int rows = model.rowCount();
    for (int row = 0; row < rows; ++row) {
        const SessionRow &sr = model.rowAt(row);
        const QString &sessionId = sr.sessionId;

        if (sr.isLoaded() && !sr.loadFailed) {
            // Blocker inspection, memoized per session until its engine state
            // may have changed
            auto memo = m_columnReports.find(sessionId);
            if (memo == m_columnReports.end() || memo->size() != columns)
                memo = m_columnReports.insert(sessionId, columnReports(sr.session.value(), m_columns));
            const QVector<BlockerReport> &reports = memo.value();

            QList<Candidate> &tier = sr.visible ? walk.visibleCandidates : walk.hiddenCandidates;
            QString name;
            for (int c = 0; c < columns; ++c) {
                const ColumnInfo &column = m_columns.at(c);
                DemandTrack track = classify(Track{sessionId, QString()}, reports.at(c), running);
                if (isListed(track.condition)) {
                    if (name.isEmpty())
                        name = model.sessionDisplayName(row);
                    track.sessionName = name;
                }

                // The last final verdict outlives the session's eviction
                const CellKey cell(sessionId, column.id);
                if (isFinal(track.condition)) {
                    m_settled.insert(cell, Settlement{track.condition, track.calculationTitles,
                                                      track.reason, track.jobFailure});
                } else {
                    m_settled.remove(cell);
                }

                if (track.condition == DemandCondition::Waiting && !m_settle->isSettling(sessionId)) {
                    for (const CalculationBlocker &blocker : reports.at(c).blockers) {
                        const PairKey key(sessionId, blocker.instanceId);
                        if (key == runningKey || m_memory.contains(key))
                            continue;
                        tier.append(Candidate{sessionId, blocker});     // deduplicated by the pass
                    }
                }
                tally(c, sessionId, track);
            }
        } else {
            // Not loaded, or a failed-load placeholder: no engine is asked
            m_columnReports.remove(sessionId);
            bool waiting = false;
            for (int c = 0; c < columns; ++c) {
                const DemandTrack track = classifyUnloaded(row, sr, m_columns.at(c));
                if (track.condition == DemandCondition::Waiting)
                    waiting = true;
                tally(c, sessionId, track);
            }
            // A visible stub is the visible loader's
            if (waiting && !sr.visible && !m_settle->isSettling(sessionId) && !held.contains(sessionId)
                && walk.loadCandidates.size() < DemandFill::kMaxHeldSessions)
                walk.loadCandidates.append(sessionId);
        }
    }
    return walk;
}

// Call under a RowStabilityGuard. An engine that holds no stored result is
// never asked, and no record is opened: see WHERE A RESULT IS LOOKED UP.
DemandTrack CalculationDemand::classifyUnloaded(int row, const SessionRow &sr, const ColumnInfo &column)
{
    DemandTrack track;
    track.sessionId = sr.sessionId;
    const QString &sessionId = sr.sessionId;

    // 1. Nothing the column needs can be stored
    if (column.storable.isEmpty()) {
        track.condition = DemandCondition::NotApplicable;
        return track;
    }

    // 2. The last final verdict of this run
    const CellKey cell(sessionId, column.id);
    const auto settled = m_settled.constFind(cell);
    if (settled != m_settled.constEnd()) {
        track.condition = settled->condition;
        track.calculationTitles = settled->titles;
        track.reason = settled->reason;
        track.jobFailure = settled->jobFailure;
        if (isListed(track.condition))
            track.sessionName = m_sessionModel->sessionDisplayName(row);
        return track;
    }

    // 3. A record of every calculation the cell needs: a result, failed when
    //    a record carries a reason
    const QSet<QString> &records = recordSet(sessionId);
    const bool everyRecord = std::all_of(column.storable.cbegin(), column.storable.cend(),
                                         [&records](const QString &id) { return records.contains(id); });
    if (everyRecord) {
        const QHash<QString, QString> reasons = m_recordReasons.value(sessionId);
        for (int i = 0; i < column.storable.size(); ++i) {
            const QString reason = reasons.value(column.storable.at(i));
            if (reason.isEmpty())
                continue;
            track.calculationTitles.append(column.storableTitles.at(i));
            if (track.reason.isEmpty())
                track.reason = QStringLiteral("%1: %2").arg(column.storableTitles.at(i), reason);
        }
        if (track.reason.isEmpty()) {
            track.condition = DemandCondition::Done;
        } else {
            track.condition = DemandCondition::Failed;
            track.sessionName = m_sessionModel->sessionDisplayName(row);
        }
        return track;
    }

    // 4. and 5. What this run remembers of the pairs
    QStringList failedTitles;
    QStringList failedReasons;
    bool allNotApplicable = true;
    for (int i = 0; i < column.storable.size(); ++i) {
        const auto memory = m_memory.constFind(PairKey(sessionId, column.storable.at(i)));
        if (memory != m_memory.constEnd() && memory->kind == Memory::Kind::JobFailed) {
            failedTitles.append(column.storableTitles.at(i));
            failedReasons.append(memory->reason);
        }
        if (memory == m_memory.constEnd() || memory->kind != Memory::Kind::NotApplicable)
            allNotApplicable = false;
    }
    if (!failedReasons.isEmpty()) {
        track.condition = DemandCondition::Failed;
        track.jobFailure = true;
        track.calculationTitles = failedTitles;
        track.reason = failedReasons.join(QStringLiteral("; "));
        track.sessionName = m_sessionModel->sessionDisplayName(row);
        return track;
    }
    if (allNotApplicable) {
        track.condition = DemandCondition::NotApplicable;
        return track;
    }

    // 6. A failed-load placeholder, visible or hidden: nothing retries its
    //    load in this run, so it is settled rather than left pending
    if (sr.isLoaded() && sr.loadFailed) {
        const Settlement failed = loadFailedSettlement();
        m_settled.insert(cell, failed);
        track.condition = failed.condition;
        track.reason = failed.reason;
        track.jobFailure = failed.jobFailure;
        track.sessionName = m_sessionModel->sessionDisplayName(row);
        return track;
    }

    // 7. In demand: the column fill loads it
    track.condition = DemandCondition::Waiting;
    return track;
}

// A load that failed: a job-level failure, settled for the run (see SETTLEMENTS)
CalculationDemand::Settlement CalculationDemand::loadFailedSettlement()
{
    return Settlement{DemandCondition::Failed, {}, tr("The session file could not be loaded"), true};
}

// The manager is asked once per session between changes of its records (a
// record change, which the manager also emits when a restore teaches it a
// record's reason), and on a model reset.
const QSet<QString> &CalculationDemand::recordSet(const QString &sessionId)
{
    const auto known = m_recordSets.constFind(sessionId);
    if (known != m_recordSets.constEnd())
        return known.value();

    ++m_recordSetLookups;
    const LogbookManager &logbook = LogbookManager::instance();
    const QSet<QString> ids = logbook.knownCalculationRecords(sessionId);
    QHash<QString, QString> reasons;
    for (const QString &id : ids) {
        const QString reason = logbook.calculationRecordReason(sessionId, id);
        if (!reason.isEmpty())
            reasons.insert(id, reason);
    }
    m_recordReasons.insert(sessionId, reasons);
    return m_recordSets.insert(sessionId, ids).value();
}

// O(columns): settlements exist for the columns of m_columns only (syncColumns()
// erases those of a column that leaves).
bool CalculationDemand::eraseSettlements(const QString &sessionId)
{
    bool erased = false;
    for (const ColumnInfo &column : std::as_const(m_columns)) {
        if (m_settled.remove(CellKey(sessionId, column.id)))
            erased = true;
    }
    return erased;
}

// A load that failed: not held, and not loaded again in this run; the
// placeholder stays in the pool and is evicted as usual
void CalculationDemand::onFillLoadFailed(const QString &sessionId)
{
    const Settlement failed = loadFailedSettlement();
    for (const ColumnInfo &column : std::as_const(m_columns))
        m_settled.insert(CellKey(sessionId, column.id), failed);
    scheduleUpdate();
}

void CalculationDemand::onFillLoaded(const QString &requestedId, const QString &heldId)
{
    if (heldId != requestedId) {
        // Corrected: the row is known by `heldId` from now on, and the manager
        // moved its records there
        m_recordSets.remove(requestedId);
        m_recordReasons.remove(requestedId);
        m_columnReports.remove(requestedId);
        eraseSettlements(requestedId);
    }
    scheduleUpdate();       // sessionLoaded scheduled a pass already
}

QStringList CalculationDemand::heldSessionIds() const
{
    return m_fill->heldSessionIds();
}

bool CalculationDemand::hasFillWork() const
{
    return m_fill->hasWork();
}

bool CalculationDemand::canLoad() const
{
    return m_fill->canLoad();
}

void CalculationDemand::runLoadStep()
{
    m_fill->step();
}

// ---- The choice -------------------------------------------------------------------------

// Tier (a), the focused track's pairs, then tier (b), the other tracks' pairs in
// row order. Within a session: plot-model order, then the report's blocker
// order (upstream first), each pair once.
// `running` is the executor's running record the walk used.
QList<CalculationDemand::Candidate> CalculationDemand::plotCandidates(const Inspections &inspections,
                                                                      const QString &focusedId,
                                                                      const JobRecord &running) const
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
    if (running.id != 0 && !running.cancelRequested)
        runningKey = PairKey(running.sessionId, running.instanceId);

    QSet<PairKey> seen;
    for (const QString &sessionId : std::as_const(sessions)) {
        if (m_settle->isSettling(sessionId))
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
// creates or answers AlreadyActive for as the chosen next job. The only place
// that offers.
void CalculationDemand::offerChoice(const QList<Candidate> &candidates)
{
    using Kind = JobQueue::OfferResult::Kind;
    if (!m_queue)
        return;

    for (const Candidate &candidate : candidates) {
        const PairKey key(candidate.sessionId, candidate.calculation.instanceId);
        const JobQueue::OfferResult result = m_queue->offer(candidate.sessionId, candidate.calculation);
        switch (result.kind) {
        case Kind::Created:
            return;
        case Kind::AlreadyActive:
            if (result.job == m_queue->chosenNextJob())
                return;     // already the choice: nothing changes
            // The running job not asked to stop: not a choice. The candidates
            // exclude its pair, so this is not expected.
            continue;
        case Kind::MissingInput:
        case Kind::NothingToDo:
        case Kind::UnknownCalculation:
            // There will never be a job for it in this run, unless its
            // session's inputs change. Column cells were classified before
            // the offers: the next pass classifies them with the memory. It
            // cannot repeat this, since a remembered pair is not offered.
            m_memory.insert(key, Memory{Memory::Kind::NotApplicable, QString()});
            scheduleUpdate();
            continue;
        case Kind::Blocked:
        case Kind::SessionNotLoaded:
            // Blocked is not expected: the candidates list upstream first (the
            // documentation says so); nothing is remembered
            continue;
        case Kind::ShuttingDown:
            return;
        }
    }

    // Nothing to choose: demand no longer wants the chosen next job
    withdrawChoice();
}

// This component is the only offerer (audit), so the chosen next job is always
// its own: nothing to choose withdraws it.
void CalculationDemand::withdrawChoice()
{
    if (m_queue)
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

// Aggregates one inspected source. `tracks` are in row order.
DemandState CalculationDemand::buildState(const QString &sourceId, const QList<DemandTrack> &tracks) const
{
    DemandState state;
    state.sourceId = sourceId;
    state.requested = true;
    for (const DemandTrack &track : tracks)
        state.addTrack(track);
    state.finish();
    return state;
}

// Stores the new states and announces the differences. `order` lists the
// inspected plots; a plot that is no longer inspected falls back to the default
// state and is announced once. The same for the requested columns, whose
// pending cells are part of what is announced.
void CalculationDemand::applyStates(const QStringList &order, const QHash<QString, DemandState> &states,
                                    const QStringList &columnOrder, const QHash<QString, DemandState> &columnStates,
                                    const QHash<QString, QSet<QString>> &pendingCells)
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

    QStringList changedColumns;
    for (const QString &id : columnOrder) {
        if (m_columnStates.value(id) != columnStates.value(id) || m_pendingCells.value(id) != pendingCells.value(id))
            changedColumns.append(id);
    }
    QStringList droppedColumns;
    for (auto it = m_columnStates.constBegin(); it != m_columnStates.constEnd(); ++it) {
        if (!columnStates.contains(it.key()))
            droppedColumns.append(it.key());
    }
    std::sort(droppedColumns.begin(), droppedColumns.end());
    changedColumns.append(droppedColumns);

    // Stored before anything is emitted: a slot reads plotState(),
    // columnState() and isCellPending()
    m_states = states;
    m_columnStates = columnStates;
    m_pendingCells = pendingCells;
    m_hasPendingCells = std::any_of(pendingCells.cbegin(), pendingCells.cend(),
                                    [](const QSet<QString> &cells) { return !cells.isEmpty(); });

    for (const QString &id : std::as_const(changedColumns))
        emit columnStateChanged(id);
    for (const QString &id : std::as_const(changed))
        emit plotStateChanged(id);
    if (!changed.isEmpty() || !changedColumns.isEmpty())
        emit statesChanged();
}

// The one pass: every path that reconciles runs it. Plot classifications are
// not cached across passes: the engine's own caches make a repeated blockers()
// cheap, and a cache keyed on dependencyChanged would be wrong (after A
// publishes, the blocker of B's output changes from A to B although B's output
// may not be re-announced). Column reports are memoized per loaded session and
// dropped by every signal after which they may differ, a job's end included.
void CalculationDemand::recompute()
{
    m_updateTimer.stop();
    ++m_passCount;

    QStringList order;
    QHash<QString, DemandState> states;
    QStringList columnOrder;
    QHash<QString, DemandState> columnStates;
    QHash<QString, QSet<QString>> pendingCells;
    {
        // The executor's signals caused by this pass's own offer or withdrawal
        // only schedule another pass
        const QScopedValueRollback<bool> reconciling(m_reconciling, true);
        m_settle->dropExpired();

        if (isInert()) {
            // Nothing is wanted: every state is the default, and nothing is held
            m_fill->update({}, {});
            withdrawChoice();
        } else {
            syncCheckedSet();
            syncColumns();

            // Ordinary plots and columns cost nothing: sessions are not even
            // enumerated for them
            const QVector<PlotValue> plots = inspectedPlots();
            const Inspections inspections = inspect(plots);

            // Cells are classified before the offers: an offer never starts a
            // job synchronously, so Running cannot change in between, and a
            // refused offer schedules the pass that classifies with it
            const JobRecord running = m_queue->job(m_queue->runningJob());
            ColumnWalk walk;
            if (!m_columns.isEmpty()) {
                const QStringList held = m_fill->heldSessionIds();
                walk = walkColumns(running, QSet<QString>(held.cbegin(), held.cend()));
            }
            // The guards are gone: from here on the session model and the
            // executor may be called. The fill releases its holds, takes the
            // load candidates and wakes the scheduler when its work changed.
            const bool shutDown = m_queue->isShutDown();
            m_fill->update(walk.pendingSessions, shutDown ? QStringList() : walk.loadCandidates);

            if (!shutDown) {
                // Tiers (a) and (b), then (c): visible sessions, then hidden
                // loaded ones. A pair is listed in its first tier only.
                QList<Candidate> candidates = plotCandidates(inspections, m_sessionModel->focusedSessionId(),
                                                             running);
                QSet<PairKey> listed;
                for (const Candidate &candidate : std::as_const(candidates))
                    listed.insert(PairKey(candidate.sessionId, candidate.calculation.instanceId));
                for (const QList<Candidate> *tier : {&walk.visibleCandidates, &walk.hiddenCandidates}) {
                    for (const Candidate &candidate : *tier) {
                        const PairKey key(candidate.sessionId, candidate.calculation.instanceId);
                        if (listed.contains(key))
                            continue;
                        listed.insert(key);
                        candidates.append(candidate);
                    }
                }
                offerChoice(candidates);
            }

            // After the offers: the executor's jobs as they are now
            if (!plots.isEmpty()) {
                const JobRecord runningNow = m_queue->job(m_queue->runningJob());
                for (const PlotValue &plot : plots) {
                    const QString id = plotId(plot);
                    QList<DemandTrack> tracks;
                    const QList<Inspection> inspected = inspections.value(id);
                    for (const Inspection &inspection : inspected)
                        tracks.append(classify(inspection.track, inspection.report, runningNow));
                    order.append(id);
                    states.insert(id, buildState(id, tracks));
                }
            }

            for (int c = 0; c < m_columns.size(); ++c) {
                const QString &id = m_columns.at(c).id;
                DemandState state = walk.states.at(c);
                state.sourceId = id;
                state.requested = true;
                state.finish();
                columnOrder.append(id);
                columnStates.insert(id, state);
                pendingCells.insert(id, walk.pendingCells.at(c));
            }
        }
    }

    applyStates(order, states, columnOrder, columnStates, pendingCells);
}

// ---- The input-settle wait ------------------------------------------------------------

void CalculationDemand::setInputSettleDelay(int milliseconds)
{
    m_settle->setDelay(milliseconds);
}

int CalculationDemand::inputSettleDelay() const
{
    return m_settle->delay();
}

void CalculationDemand::endInputSettleWaits()
{
    m_settle->endAll();
    scheduleUpdate();
}

bool CalculationDemand::isSettling(const QString &sessionId) const
{
    return m_settle->isSettling(sessionId);
}

bool CalculationDemand::hasSettlingSessions() const
{
    return m_settle->hasSettling();
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

// The bulk edit publishes its edit this way too, for a loaded session and for
// a stub: it is an input change like any other.
void CalculationDemand::onDependencyChanged(const QString &sessionId, const DependencyKey &key)
{
    // The session's engine state changed: its column reports are inspected again
    m_columnReports.remove(sessionId);

    // Only the static closure of the checked requested plots and of the
    // requested columns matters: any other edit neither delays nor retries
    // anything
    if (!isRelevantName(key))
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
    m_memory.removeIf([&sessionId](QHash<PairKey, Memory>::iterator it) { return it.key().first == sessionId; });
    eraseSettlements(sessionId);
    m_settle->start(sessionId);
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

// Whatever a pass reads of the rows and the columns is read again: a pass that
// runs inside the reset's delivery (the executor's jobFinished) sees the new
// columns and rows.
void CalculationDemand::onSessionModelAboutToBeReset()
{
    m_columnReports.clear();
    m_recordSets.clear();
    m_recordReasons.clear();
}

// A sort resets the model too: only the ids that no longer have a row are
// forgotten, so that a job failure is not retried and a settled session is not
// loaded again on every sort. A column change resets the model as well.
void CalculationDemand::onSessionModelReset()
{
    // Again: a pass between the two signals read the rows mid-reset
    onSessionModelAboutToBeReset();

    if (m_sessionModel) {
        const SessionModel &model = *m_sessionModel;
        QSet<QString> ids;
        const int rows = model.rowCount();
        ids.reserve(rows);
        for (int row = 0; row < rows; ++row)
            ids.insert(model.rowAt(row).sessionId);

        m_memory.removeIf([&ids](QHash<PairKey, Memory>::iterator it) { return !ids.contains(it.key().first); });
        m_settle->keepOnly(ids);
        m_settled.removeIf([&ids](QHash<CellKey, Settlement>::iterator it) { return !ids.contains(it.key().first); });
    }
    scheduleUpdate();
}

// Inside a record method (a write, a removal, a skip, possibly from an engine
// listener): only drops state and schedules.
void CalculationDemand::onCalculationRecordsChanged(const QString &sessionId, const QString &calculationId)
{
    m_recordSets.remove(sessionId);
    m_recordReasons.remove(sessionId);
    m_columnReports.remove(sessionId);
    for (const ColumnInfo &column : std::as_const(m_columns)) {
        if (column.calculations.contains(calculationId))
            m_settled.remove(CellKey(sessionId, column.id));
    }
    scheduleUpdate();
}

void CalculationDemand::onJobFinished(JobId id, JobState state)
{
    // Read first: the record may be trimmed once this slot has returned
    const JobRecord record = m_queue ? m_queue->job(id) : JobRecord();
    if (record.id != 0)
        m_columnReports.remove(record.sessionId);
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
    // Synchronously: the executor emits jobFinished before it schedules the
    // next start, so the next choice is in place first
    recompute();
}

// Text only, without inspection: optimizer iterations can arrive many times
// per second. A Running track exists only for the running job not asked to
// stop, of which there is one, and the executor reports the progress of its
// running job only: the running tracks of the job's session are the job's.
void CalculationDemand::onJobProgress(JobId id, const QString &text)
{
    const JobRecord record = m_queue ? m_queue->job(id) : JobRecord();
    if (record.id == 0 || record.state != JobState::Running)
        return;                         // not a job any track can describe

    const auto update = [&record, &text](QHash<QString, DemandState> &states) {
        QStringList changed;
        for (auto it = states.begin(); it != states.end(); ++it) {
            DemandState &state = it.value();
            bool touched = false;
            for (DemandTrack &track : state.running) {
                if (track.sessionId != record.sessionId || track.progressText == text)
                    continue;
                track.progressText = text;
                touched = true;
            }
            if (!touched)
                continue;
            state.toolTip = DemandState::buildToolTip(state);
            changed.append(it.key());
        }
        std::sort(changed.begin(), changed.end());
        return changed;
    };
    const QStringList changedColumns = update(m_columnStates);
    const QStringList changed = update(m_states);

    for (const QString &columnId : changedColumns)
        emit columnStateChanged(columnId);
    for (const QString &plotId : changed)
        emit plotStateChanged(plotId);
    if (!changed.isEmpty() || !changedColumns.isEmpty())
        emit statesChanged();
}

void CalculationDemand::onRegistryChanged()
{
    m_requested.clear();
    m_staticNames.clear();
    m_memory.clear();
    // What a column needs follows the registrations: the next pass reads it
    // from the session model
    m_settled.clear();
    m_columnReports.clear();
    scheduleUpdate();
}

} // namespace FlySight
