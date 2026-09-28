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
        // load restored the session's stored results. A load that succeeded
        // makes what this run remembered of the session's failed loads false;
        // a failed-load placeholder is announced too, and forgets nothing.
        connect(m_sessionModel, &SessionModel::sessionLoaded, this, [this](const QString &sessionId) {
            dropReports(sessionId);
            const int row = m_sessionModel->getSessionRow(sessionId);
            if (row >= 0) {
                const SessionRow &sr = std::as_const(*m_sessionModel).rowAt(row);
                if (sr.isLoaded() && !sr.loadFailed)
                    forgetSession(sessionId, Forget::LoadFailures);
            }
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
        // Direct: the model relays the result store's announcement from inside
        // the engine's explicit-result listener, so the slot only records and
        // schedules, like the record-change slot below. The manager's record
        // change of the pair comes first.
        connect(m_sessionModel, &SessionModel::calculationRecordWriteFailed,
                this, &CalculationDemand::onCalculationRecordWriteFailed, Qt::DirectConnection);
    }

    // Direct: emitted from inside record methods, so the slot only drops state
    // and schedules, like SessionModel's own
    connect(&LogbookManager::instance(), &LogbookManager::calculationRecordsChanged,
            this, &CalculationDemand::onCalculationRecordsChanged, Qt::DirectConnection);

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

DemandProgress CalculationDemand::progress() const
{
    return m_progress;
}

QList<SessionFailures> CalculationDemand::failures() const
{
    return m_failures;
}

// Asked for every painted logbook row: an index lookup, never a scan
SessionFailures CalculationDemand::sessionFailures(const QString &sessionId) const
{
    const auto index = m_failureIndex.constFind(sessionId);
    if (index == m_failureIndex.constEnd())
        return SessionFailures{sessionId, QString(), {}};
    return m_failures.at(index.value());
}

bool CalculationDemand::isInert() const
{
    return !m_sessionModel || !m_plotModel || !m_queue;
}

// ---- Which plots and columns matter -----------------------------------------------

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

// The sources as they are now: a per-pass copy. A plot's requested
// calculations come from the registry (the model knows nothing of plots), a
// column's from the session model (a column is requested when they are not
// empty); both are read, not computed here. Nothing is compared with the last
// pass: the report memo is keyed by source id, so it is right for any list.
void CalculationDemand::syncSources()
{
    QVector<Source> sources;
    const CalculationRegistry &registry = CalculationRegistry::instance();

    for (const QString &id : std::as_const(m_checkedOrder)) {
        const PlotValue plot = m_checked.value(id);
        if (!isRequested(plot))
            continue;
        Source source;
        source.kind = Source::Kind::Plot;
        source.id = id;
        source.names = {yName(plot)};
        source.sensorId = plot.sensorID;
        setStorable(source, registry.explicitDependencies(yName(plot)));
        sources.append(source);
    }

    if (m_sessionModel) {
        const SessionModel &model = *m_sessionModel;
        const int count = model.columnCount();
        for (int i = 0; i < count; ++i) {
            const QStringList calculations = model.columnRequestedCalculations(i);
            if (calculations.isEmpty())
                continue;
            const LogbookColumn &column = model.column(i);
            Source source;
            source.kind = Source::Kind::Column;
            source.id = columnId(column);
            source.names = logbookColumnNames(column);
            setStorable(source, calculations);
            sources.append(source);
        }
    }

    m_sources = sources;
}

void CalculationDemand::setStorable(Source &source, const QStringList &calculations)
{
    const CalculationRegistry &registry = CalculationRegistry::instance();
    source.storable.clear();
    source.storableTitles.clear();
    for (const QString &id : calculations) {
        // An explicit family instance is never stored
        if (id.contains(QLatin1Char('#')))
            continue;
        const QString title = registry.title(id);
        source.storable.append(id);
        source.storableTitles.append(title.isEmpty() ? id : title);
    }
}

// ---- Inspection and classification ---------------------------------------------------

bool CalculationDemand::isPending(TrackCondition condition)
{
    return condition == TrackCondition::Waiting || condition == TrackCondition::Running;
}

namespace {

BlockerReport::State blockerState(const SessionData &session, const QString &sensorId, const QString &measurementId)
{
    return session.calculationEngine().blockers(DependencyKey::measurement(sensorId, measurementId)).state;
}

} // namespace

bool CalculationDemand::isMerelyUncomputed(const SessionData &session, const QString &sensorId,
                                           const QString &measurementId)
{
    using State = BlockerReport::State;
    const State state = blockerState(session, sensorId, measurementId);
    return state == State::Blocked || state == State::NotProduced;
}

bool CalculationDemand::isNotYetComputed(const SessionData &session, const QString &sensorId,
                                         const QString &measurementId)
{
    return blockerState(session, sensorId, measurementId) == BlockerReport::State::Blocked;
}

// Call under the walk's RowStabilityGuard: `session` is a row of the model read
// in place. One report per source, combined over its names (two for a Delta
// column): a value that needs both names exists only when both do. A plot's
// single name combines to itself, except that an Available or NotApplicable
// report keeps nothing, a NotProduced one keeps its notes only and a Blocked
// one its blockers only.
BlockerReport CalculationDemand::combinedReport(const SessionData &session, const Source &source)
{
    using State = BlockerReport::State;
    auto &engine = session.calculationEngine();

    bool notApplicable = false;
    bool notProduced = false;
    bool blocked = false;
    BlockerReport combined;
    QSet<QString> seenBlockers;
    QSet<QString> seenNotes;
    for (const DependencyKey &name : source.names) {
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

#ifndef QT_NO_DEBUG
    // "y available implies x available" (see the class comment) is a property
    // of the registrations that nothing else checks. Which time axis is drawn
    // is a view setting, so the registry cannot check it at registration; both
    // axes are inspected here instead. blockers() never starts explicit work
    // and never loads a session. A time axis that is unavailable for ordinary
    // reasons is not a violation: only one that waits on an explicit
    // calculation would leave a Done track undrawn.
    if (source.kind == Source::Kind::Plot && combined.state == State::Available) {
        for (const char *axis : {SessionKeys::Time, SessionKeys::SystemTime}) {
            const DependencyKey xName = DependencyKey::measurement(source.sensorId, QLatin1String(axis));
            if (engine.blockers(xName).state == State::Blocked) {
                qWarning().noquote() << "CalculationDemand:" << source.id
                                     << "is available but its time axis" << QLatin1String(axis)
                                     << "waits on an explicit calculation";
            }
        }
    }
#endif

    return combined;
}

// A failure always says why: a remembered why and an entry's reason are never
// empty, whatever their source gave
QString CalculationDemand::failureDetail(const QString &detail, bool computationFailed)
{
    if (!detail.isEmpty())
        return detail;
    return computationFailed ? tr("Calculation failed") : tr("No result for this session");
}

QString CalculationDemand::noteDetail(const UnproducedNote &note)
{
    return failureDetail(note.detail, note.status == ResultStatus::Failed);
}

QString CalculationDemand::loadFailureReason()
{
    return tr("The session file could not be loaded");
}

// The memory keeps the why alone, so that an entry shows it beside the title
// without parsing text. Nothing remembered is stored, so the next start tries
// every one again.
CalculationDemand::TrackCondition CalculationDemand::failedTrack(const QStringList &calculationIds,
                                                                 const QStringList &titles,
                                                                 const QList<const PairMemory *> &facts,
                                                                 QList<FailedCalculation> *failures)
{
    for (int i = 0; i < facts.size(); ++i)
        failures->append(FailedCalculation{calculationIds.at(i), titles.at(i), facts.at(i)->reason, true});
    return TrackCondition::Failed;
}

// A loaded row that is not a placeholder. `running` is the executor's running
// record (a default record when there is none). Reads the memory; facts go to
// `learned` and are applied after the walk. Every branch that makes the track
// Failed appends its entries to `failures` in the same place.
CalculationDemand::TrackCondition CalculationDemand::classifyLoaded(const QString &sessionId, const Source &source,
                                                                    const BlockerReport &report,
                                                                    const JobRecord &running,
                                                                    QList<LearnedFact> *learned,
                                                                    QList<FailedCalculation> *failures) const
{
    // A column found not applicable remembers it for itself, for every
    // storable calculation not already kept from being offered: after its
    // eviction the session is not loaded again for this column
    const auto learnColumnVerdict = [&] {
        if (source.kind != Source::Kind::Column)
            return;
        for (const QString &id : source.storable) {
            if (!blocksOffer(remembered(sessionId, id)))
                learned->append(LearnedFact{sessionId, id, PairMemory::columnVerdict(source.id)});
        }
    };

    switch (report.state) {
    case BlockerReport::State::Available: {
        // Done only when none of the storable calculations is remembered
        // failed: in the product only a record that could not be written
        // meets this (a failed job or an unstored result leaves no result, a
        // failed load is forgotten when the session loads)
        QStringList ids;
        QStringList titles;
        QList<const PairMemory *> facts;
        for (int i = 0; i < source.storable.size(); ++i) {
            const PairMemory *memory = remembered(sessionId, source.storable.at(i));
            if (memory && memory->kind == PairMemory::Kind::Failed) {
                ids.append(source.storable.at(i));
                titles.append(source.storableTitles.at(i));
                facts.append(memory);
            }
        }
        if (!facts.isEmpty())
            return failedTrack(ids, titles, facts, failures);
        return TrackCondition::Done;
    }
    case BlockerReport::State::NotApplicable:
        learnColumnVerdict();
        return TrackCondition::NotApplicable;
    case BlockerReport::State::NotProduced:
        for (const UnproducedNote &note : report.notProduced) {
            const QString &id = note.calculation.instanceId;
            const bool storable = source.storable.contains(id);
            // A result that is never stored (a computation that threw) would
            // be computed again after every eviction: remembered failed
            if (note.status != ResultStatus::Ok && storable) {
                learned->append(LearnedFact{sessionId, id,
                                            PairMemory::failed(PairMemory::Origin::Result, noteDetail(note))});
            }
            // Stored only when it is an Ok result of a storable calculation
            // whose record did not fail to be written: a stored rejection is
            // not tried again, anything else is
            const PairMemory *memory = remembered(sessionId, id);
            const bool stored = note.status == ResultStatus::Ok && storable
                && !(memory && memory->kind == PairMemory::Kind::Failed);
            failures->append(FailedCalculation{id, titleOf(note.calculation), noteDetail(note), !stored});
        }
        return TrackCondition::Failed;
    case BlockerReport::State::Blocked:
        break;      // notes on a Blocked report are ignored: Blocked wins
    }

    const bool runningIsLive = running.id != 0 && !running.cancelRequested && running.sessionId == sessionId;
    bool isRunning = false;
    bool allRefused = true;
    QStringList failedIds;
    QStringList failedTitles;
    QList<const PairMemory *> facts;
    for (const CalculationBlocker &blocker : report.blockers) {
        if (runningIsLive && running.instanceId == blocker.instanceId)
            isRunning = true;

        // A column verdict is never read here: the engine is the authority
        // for a loaded session
        const PairMemory *memory = remembered(sessionId, blocker.instanceId);
        if (memory && memory->kind == PairMemory::Kind::Failed) {
            failedIds.append(blocker.instanceId);
            failedTitles.append(titleOf(blocker));
            facts.append(memory);
        }
        if (!memory || memory->origin != PairMemory::Origin::Refused)
            allRefused = false;
    }

    if (isRunning)
        return TrackCondition::Running;
    if (!facts.isEmpty()) {
        // Not offered again in this run: the failure entries say why
        return failedTrack(failedIds, failedTitles, facts, failures);
    }
    if (allRefused) {
        // The executor said there is nothing to run for any of them
        learnColumnVerdict();
        return TrackCondition::NotApplicable;
    }
    return TrackCondition::Waiting;
}

// Call under the walk's RowStabilityGuard. An engine that holds no stored
// result is never asked, and no record is opened: see WHERE A RESULT IS
// LOOKED UP. The memory is read before the manager. Every rule that makes the
// track Failed appends its entries to `failures` in the same place.
CalculationDemand::TrackCondition CalculationDemand::classifyUnloaded(const SessionRow &sr, const Source &source,
                                                                      QList<LearnedFact> *learned,
                                                                      QList<FailedCalculation> *failures)
{
    const QString &sessionId = sr.sessionId;

    // 1. Nothing the column needs can be stored
    if (source.storable.isEmpty())
        return TrackCondition::NotApplicable;

    // 2. and 3. What this run remembers of the pairs
    QStringList failedIds;
    QStringList failedTitles;
    QList<const PairMemory *> facts;
    bool allNotApplicable = true;
    for (int i = 0; i < source.storable.size(); ++i) {
        const PairMemory *memory = remembered(sessionId, source.storable.at(i));
        if (memory && memory->kind == PairMemory::Kind::Failed) {
            failedIds.append(source.storable.at(i));
            failedTitles.append(source.storableTitles.at(i));
            facts.append(memory);
        }
        if (!notApplicableFor(memory, source.id))
            allNotApplicable = false;
    }
    if (!facts.isEmpty())
        return failedTrack(failedIds, failedTitles, facts, failures);
    if (allNotApplicable)
        return TrackCondition::NotApplicable;

    // 4. A record of every calculation the cell needs: a result, failed when
    //    a record carries a reason. Every pair with a reason is an entry, a
    //    stored rejection, read from the memo beside the record set.
    const QSet<QString> &records = recordSet(sessionId);
    const bool everyRecord = std::all_of(source.storable.cbegin(), source.storable.cend(),
                                         [&records](const QString &id) { return records.contains(id); });
    if (everyRecord) {
        const QHash<QString, QString> reasons = m_recordReasons.value(sessionId);
        bool failed = false;
        for (int i = 0; i < source.storable.size(); ++i) {
            const QString reason = reasons.value(source.storable.at(i));
            if (reason.isEmpty())
                continue;
            failed = true;
            failures->append(FailedCalculation{source.storable.at(i), source.storableTitles.at(i), reason, false});
        }
        return failed ? TrackCondition::Failed : TrackCondition::Done;
    }

    // 5. A failed-load placeholder, visible or hidden: nothing retries its
    //    load in this run, so it is remembered failed rather than left
    //    pending. The track and its entries are exactly what rule 2 gives
    //    once the facts are applied, so the next pass announces nothing.
    if (sr.isLoaded() && sr.loadFailed) {
        const PairMemory failed = PairMemory::failed(PairMemory::Origin::Load, loadFailureReason());
        QList<const PairMemory *> loadFacts;
        for (const QString &id : source.storable) {
            learned->append(LearnedFact{sessionId, id, failed});
            loadFacts.append(&failed);
        }
        return failedTrack(source.storable, source.storableTitles, loadFacts, failures);
    }

    // 6. In demand: the column fill loads it
    return TrackCondition::Waiting;
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

void CalculationDemand::dropReports(const QString &sessionId)
{
    m_reports.remove(sessionId);
    ++m_reportDrops;
}

void CalculationDemand::dropAllReports()
{
    m_reports.clear();
    ++m_reportDrops;
}

// ---- The pair memory ------------------------------------------------------------------

const CalculationDemand::PairMemory *CalculationDemand::remembered(const QString &sessionId,
                                                                   const QString &calculationId) const
{
    const auto session = m_memory.constFind(sessionId);
    if (session == m_memory.constEnd())
        return nullptr;
    const auto fact = session->constFind(calculationId);
    return fact == session->constEnd() ? nullptr : &fact.value();
}

bool CalculationDemand::blocksOffer(const PairMemory *memory)
{
    return memory && (memory->kind == PairMemory::Kind::Failed || memory->origin == PairMemory::Origin::Refused);
}

bool CalculationDemand::notApplicableFor(const PairMemory *memory, const QString &columnId)
{
    return memory && memory->kind == PairMemory::Kind::NotApplicable
        && (memory->origin == PairMemory::Origin::Refused || memory->columns.contains(columnId));
}

void CalculationDemand::remember(const QString &sessionId, const QString &calculationId, const PairMemory &fact)
{
    QHash<QString, PairMemory> &facts = m_memory[sessionId];
    const auto known = facts.find(calculationId);
    if (known == facts.end()) {
        facts.insert(calculationId, fact);
        return;
    }

    PairMemory &memory = known.value();
    if (fact.kind == PairMemory::Kind::Failed) {
        memory = fact;
    } else if (fact.origin == PairMemory::Origin::Refused) {
        if (memory.kind != PairMemory::Kind::Failed)
            memory = fact;
    } else if (memory.origin == PairMemory::Origin::ColumnVerdict) {
        memory.columns.unite(fact.columns);
    }
}

bool CalculationDemand::forgetSession(const QString &sessionId, Forget which)
{
    if (which == Forget::Everything)
        return m_memory.remove(sessionId);

    const auto session = m_memory.find(sessionId);
    if (session == m_memory.end())
        return false;
    const qsizetype forgotten = session->removeIf([](QHash<QString, PairMemory>::iterator it) {
        return it.value().origin == PairMemory::Origin::Load;
    });
    if (session->isEmpty())
        m_memory.erase(session);
    return forgotten > 0;
}

bool CalculationDemand::forgetPair(const QString &sessionId, const QString &calculationId)
{
    const auto session = m_memory.find(sessionId);
    if (session == m_memory.end())
        return false;
    const bool forgotten = session->remove(calculationId);
    if (session->isEmpty())
        m_memory.erase(session);
    return forgotten;
}

// A load that failed: not held, and not loaded again in this run; the
// placeholder stays in the pool and is evicted as usual
void CalculationDemand::onFillLoadFailed(const QString &sessionId)
{
    const PairMemory failed = PairMemory::failed(PairMemory::Origin::Load, loadFailureReason());
    for (const Source &source : std::as_const(m_sources)) {
        if (source.kind != Source::Kind::Column)
            continue;
        for (const QString &id : source.storable)
            remember(sessionId, id, failed);
    }
    scheduleUpdate();
}

void CalculationDemand::onFillLoaded(const QString &requestedId, const QString &heldId)
{
    if (heldId != requestedId) {
        // Corrected: the row is known by `heldId` from now on, and the manager
        // moved its records there
        m_recordSets.remove(requestedId);
        m_recordReasons.remove(requestedId);
        dropReports(requestedId);
        forgetSession(requestedId, Forget::Everything);
    }
    scheduleUpdate();       // coalesces with the pass sessionLoaded scheduled
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

// ---- The walk and the choice -------------------------------------------------------------

// The one guarded read of the component. For each row and each source whose
// tracks include it: the track is classified, its condition feeds the row's
// progress, failures and pending cells, and, for a loaded session, its
// report's blockers are filed by tier. Everything returned is a plain value,
// so that nothing is offered, withdrawn, loaded, pinned or emitted while the
// guard is held; the walk writes the report and record-set memos only, and
// the facts it learns go to the pass. blockers() may compute on-demand
// values; that reads, it neither loads nor evicts. blockers() and the display
// name may deliver the engine's pending events; the slots they reach here
// only drop memos and schedule, so the walk holds no reference into its memos
// across them.
CalculationDemand::Walk CalculationDemand::walkRows(const JobRecord &running, const QSet<QString> &held,
                                                    const QString &focusedId)
{
    Walk walk;
    const int sources = int(m_sources.size());
    walk.pendingCells.resize(sources);

    // A job asked to stop is winding down: it is described by nothing
    const bool runningIsLive = running.id != 0 && !running.cancelRequested;
    PairKey runningKey;
    if (runningIsLive)
        runningKey = PairKey(running.sessionId, running.instanceId);

    const SessionModel &model = *m_sessionModel;
    const SessionModel::RowStabilityGuard guard(model);
    const int rows = model.rowCount();
    for (int row = 0; row < rows; ++row) {
        const SessionRow &sr = model.rowAt(row);
        const QString &sessionId = sr.sessionId;
        const bool loaded = sr.isLoaded() && !sr.loadFailed;
        const bool settling = m_settle->isSettling(sessionId);
        QString name;                   // read for a row with failures, or the running job's row
        bool named = false;
        const auto rowName = [&] {
            if (!named) {
                name = model.sessionDisplayName(row);
                named = true;
            }
            return name;
        };
        bool waiting = false;           // a column track of a row that is not loaded is waiting
        bool inDemand = false;          // some track of the row, in any source, is Waiting or Running
        QList<FailedCalculation> rowFailures;   // each pair once: the first source that fails it
        if (!loaded)
            dropReports(sessionId);

        for (int s = 0; s < sources; ++s) {
            const Source &source = m_sources.at(s);
            const bool isPlot = source.kind == Source::Kind::Plot;
            // A plot's tracks are the rows the plot widget draws
            if (isPlot && !(loaded && sr.visible))
                continue;

            TrackCondition condition = TrackCondition::NotApplicable;
            QList<FailedCalculation> entries;       // the track's, when it is Failed
            if (loaded) {
                // Blocker inspection, memoized per session until its engine
                // state may have changed. A copy, never a reference into the
                // memos (see above); a report whose memos were dropped while
                // it was computed is used by this pass but not memoized.
                BlockerReport report;
                const auto memos = m_reports.constFind(sessionId);
                if (memos != m_reports.constEnd() && memos->contains(source.id)) {
                    report = memos->value(source.id);
                } else {
                    const quint64 drops = m_reportDrops;
                    report = combinedReport(sr.session.value(), source);
                    if (m_reportDrops == drops)
                        m_reports[sessionId].insert(source.id, report);
                }
                condition = classifyLoaded(sessionId, source, report, running, &walk.learned, &entries);

                // Every blocker that may be offered, whatever the track's
                // condition; a settling session is offered once its inputs
                // have been still for the whole wait
                if (report.state == BlockerReport::State::Blocked && !settling) {
                    const Walk::Tier tier = isPlot ? (sessionId == focusedId ? Walk::FocusedPlots : Walk::OtherPlots)
                                                   : (sr.visible ? Walk::VisibleColumns : Walk::HiddenColumns);
                    for (const CalculationBlocker &blocker : report.blockers) {
                        if (PairKey(sessionId, blocker.instanceId) == runningKey
                            || blocksOffer(remembered(sessionId, blocker.instanceId)))
                            continue;
                        walk.tiers[tier].append(Candidate{sessionId, blocker});
                    }
                }
            } else {
                // Not loaded, or a failed-load placeholder: no engine is asked
                condition = classifyUnloaded(sr, source, &walk.learned, &entries);
                if (condition == TrackCondition::Waiting)
                    waiting = true;
            }

            if (isPending(condition)) {
                inDemand = true;
                if (!isPlot) {
                    walk.pendingCells[s].insert(sessionId);
                    walk.pendingSessions.insert(sessionId);
                }
            }
            for (const FailedCalculation &entry : std::as_const(entries)) {
                const bool known = std::any_of(rowFailures.cbegin(), rowFailures.cend(),
                                               [&entry](const FailedCalculation &failure) {
                                                   return failure.calculationId == entry.calculationId;
                                               });
                if (!known)
                    rowFailures.append(entry);
            }
        }

        // Each session counts once, however many of its tracks are in demand
        if (inDemand)
            ++walk.progress.count;
        if (!rowFailures.isEmpty())
            walk.failures.append(SessionFailures{sessionId, rowName(), rowFailures});
        if (runningIsLive && sessionId == running.sessionId) {
            walk.progress.sessionName = rowName();
            walk.progress.progressText = running.progressText;
            walk.progressJob = running.id;
        }

        // A visible stub is the visible loader's
        if (waiting && !sr.visible && !settling && !held.contains(sessionId)
            && walk.loadCandidates.size() < DemandFill::kMaxHeldSessions)
            walk.loadCandidates.append(sessionId);
    }
    return walk;
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
            // session's inputs change. Every track was classified in the walk,
            // before the offers: the next pass classifies them with the
            // memory. It cannot repeat this, since a refused pair is not
            // offered.
            remember(candidate.sessionId, candidate.calculation.instanceId, PairMemory::refused());
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

// Each value is compared with the last pass's and announced only when it
// differs. A column that is no longer requested is found among the old keys
// of the pending cells, and changes what a painted cell shows only when it had
// a pending cell.
void CalculationDemand::applyValues(const QStringList &columnOrder, const QHash<QString, QSet<QString>> &pendingCells,
                                    const DemandProgress &progress, JobId progressJob,
                                    const QList<SessionFailures> &failures)
{
    QStringList changedCells;
    for (const QString &id : columnOrder) {
        if (m_pendingCells.value(id) != pendingCells.value(id))
            changedCells.append(id);
    }
    QStringList droppedCells;
    for (auto it = m_pendingCells.constBegin(); it != m_pendingCells.constEnd(); ++it) {
        if (!pendingCells.contains(it.key()) && !it.value().isEmpty())
            droppedCells.append(it.key());
    }
    std::sort(droppedCells.begin(), droppedCells.end());
    changedCells.append(droppedCells);

    const bool progressChangedNow = m_progress != progress;
    const bool failuresChangedNow = m_failures != failures;

    // Stored before anything is emitted: a slot of any signal reads
    // isCellPending(), progress(), failures() and sessionFailures()
    m_pendingCells = pendingCells;
    m_hasPendingCells = std::any_of(pendingCells.cbegin(), pendingCells.cend(),
                                    [](const QSet<QString> &cells) { return !cells.isEmpty(); });
    m_progress = progress;
    m_progressJob = progressJob;
    if (failuresChangedNow) {
        m_failures = failures;
        m_failureIndex.clear();
        for (qsizetype i = 0; i < m_failures.size(); ++i)
            m_failureIndex.insert(m_failures.at(i).sessionId, i);
    }

    for (const QString &id : std::as_const(changedCells))
        emit pendingCellsChanged(id);
    if (progressChangedNow)
        emit progressChanged();
    if (failuresChangedNow)
        emit failuresChanged();
}

// The one pass: every path that reconciles runs it. One walk classifies every
// track, gathers the progress, the failures and the pending cells, and files
// every candidate under one guard; the offers, the fill, the memory and the
// signals come after it. The reports it reads are memoized per loaded session
// (see WHAT A PASS COSTS): any dependency change of the session drops its
// whole entry, and so do a job's end and a load, so the publication of A
// drops the report of B's output whose blocker changes from A to B, although
// B's output may not be re-announced.
void CalculationDemand::recompute()
{
    m_updateTimer.stop();
    ++m_passCount;

    QStringList columnOrder;
    QHash<QString, QSet<QString>> pendingCells;
    DemandProgress progress;            // the default when inert
    JobId progressJob = 0;
    QList<SessionFailures> failures;    // none when inert
    {
        // The executor's signals caused by this pass's own offer or withdrawal
        // only schedule another pass
        const QScopedValueRollback<bool> reconciling(m_reconciling, true);
        m_settle->dropExpired();

        if (isInert()) {
            // Nothing is wanted: nothing to compute, no failure, no pending
            // cell, and nothing is held
            m_fill->update({}, {});
            withdrawChoice();
        } else {
            syncCheckedSet();
            syncSources();

            // Read before the guard. Every track is classified before the
            // offers: an offer never starts a job synchronously, so Running
            // cannot change in between, and a refused offer schedules the
            // pass that classifies with it.
            const JobRecord running = m_queue->job(m_queue->runningJob());
            const QStringList held = m_fill->heldSessionIds();
            const QString focused = m_sessionModel->focusedSessionId();
            // Ordinary plots and columns cost nothing: with no source, rows
            // are not even walked
            const Walk walk = m_sources.isEmpty()
                ? Walk()
                : walkRows(running, QSet<QString>(held.cbegin(), held.cend()), focused);

            // The facts change no track of this pass (a NotApplicable or
            // NotProduced track stays so, and a placeholder's track is already
            // the one the facts give), so no further pass is needed
            for (const LearnedFact &learned : walk.learned)
                remember(learned.sessionId, learned.calculationId, learned.fact);

            // The guard is gone: from here on the session model and the
            // executor may be called. The fill releases its holds, takes the
            // load candidates and wakes the scheduler when its work changed.
            const bool shutDown = m_queue->isShutDown();
            m_fill->update(walk.pendingSessions, shutDown ? QStringList() : walk.loadCandidates);

            if (!shutDown) {
                // The tiers in order. A pair is listed in its first tier only.
                QList<Candidate> candidates;
                QSet<PairKey> listed;
                for (const QList<Candidate> &tier : walk.tiers) {
                    for (const Candidate &candidate : tier) {
                        const PairKey key(candidate.sessionId, candidate.calculation.instanceId);
                        if (listed.contains(key))
                            continue;
                        listed.insert(key);
                        candidates.append(candidate);
                    }
                }
                offerChoice(candidates);
            }

            // Every requested column has a set, empty when no cell is pending
            for (int s = 0; s < m_sources.size(); ++s) {
                const Source &source = m_sources.at(s);
                if (source.kind != Source::Kind::Column)
                    continue;
                columnOrder.append(source.id);
                pendingCells.insert(source.id, walk.pendingCells.value(s));
            }

            // The high-water mark is the pass's: a new burst of work after the
            // count reached 0 starts at its own count
            progress = walk.progress;
            progress.highWater = progress.count == 0 ? 0 : std::max(m_progress.highWater, progress.count);
            progressJob = walk.progressJob;
            failures = walk.failures;
        }
    }

    applyValues(columnOrder, pendingCells, progress, progressJob, failures);
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
    // The session's engine state changed: its reports are inspected again
    dropReports(sessionId);

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
    // possible now, and the session waits until its inputs are still. This
    // is the only clearing of a session that is not loaded: no display
    // change of the model is observed.
    forgetSession(sessionId, Forget::Everything);
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
    dropAllReports();
    m_recordSets.clear();
    m_recordReasons.clear();
}

// A sort resets the model too: only the ids that no longer have a row are
// forgotten, so that a failure is not retried and a session found not
// applicable is not loaded again on every sort. A column change resets the
// model as well.
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

        m_memory.removeIf([&ids](QHash<QString, QHash<QString, PairMemory>>::iterator it) {
            return !ids.contains(it.key());
        });
        m_settle->keepOnly(ids);
    }
    scheduleUpdate();
}

// Inside a record method (a write, a removal, a skip, possibly from an engine
// listener): only drops state and schedules. A record change of the pair
// forgets what was remembered of it: a later successful write clears a write
// that failed. On the failing write itself this comes first, and the
// write-failed announcement that follows remembers the failure.
void CalculationDemand::onCalculationRecordsChanged(const QString &sessionId, const QString &calculationId)
{
    m_recordSets.remove(sessionId);
    m_recordReasons.remove(sessionId);
    dropReports(sessionId);
    forgetPair(sessionId, calculationId);
    scheduleUpdate();
}

// Inside the engine's explicit-result listener: only records and schedules.
// The result is installed but its record could not be written: a failure of
// this run, not offered again until the session's inputs change.
void CalculationDemand::onCalculationRecordWriteFailed(const QString &sessionId, const QString &calculationId,
                                                       const QString &reason)
{
    remember(sessionId, calculationId,
             PairMemory::failed(PairMemory::Origin::Write, failureDetail(reason, true)));
    scheduleUpdate();
}

void CalculationDemand::onJobFinished(JobId id, JobState state)
{
    // Read first: the record may be trimmed once this slot has returned
    const JobRecord record = m_queue ? m_queue->job(id) : JobRecord();
    if (record.id != 0)
        dropReports(record.sessionId);
    if (state == JobState::Failed && record.id != 0) {
        // Not a function of the inputs, and not stored: not offered again in
        // this run unless the session's inputs change
        remember(record.sessionId, record.instanceId,
                 PairMemory::failed(PairMemory::Origin::Job, failureDetail(record.reason, true)));
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
// per second. The progress value describes the running job not asked to stop
// that the last pass found, of which there is one, and the executor reports
// the progress of its running job only: only that job's text changes it.
void CalculationDemand::onJobProgress(JobId id, const QString &text)
{
    const JobRecord record = m_queue ? m_queue->job(id) : JobRecord();
    if (record.id == 0 || record.state != JobState::Running)
        return;                         // not a job the progress can describe
    if (id != m_progressJob || record.cancelRequested || m_progress.progressText == text)
        return;

    // Stored before it is emitted, like a pass
    m_progress.progressText = text;
    emit progressChanged();
}

void CalculationDemand::onRegistryChanged()
{
    m_requested.clear();
    m_staticNames.clear();
    // What a source needs follows the registrations: the next pass reads it
    // again
    m_memory.clear();
    dropAllReports();
    scheduleUpdate();
}

} // namespace FlySight
